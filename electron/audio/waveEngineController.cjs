const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const { calculatePatternGains, normalizePattern } = require('./wavePatterns.cjs');
const {
  calculateSpatialGains,
  calculateOrbitPosition,
  calculateHorizontalSweepPosition,
  calculateVerticalSweepPosition,
  getDefaultSpeakerPositions
} = require('./spatialAudio.cjs');
const {
  normalizeMotionMode,
  normalizeCurveType,
  calculateSequentialGains,
  calculateSmoothTravelGains,
  calculateCircularTravelGains,
  calculateBounceGains,
  calculateShuffleGains,
  calculatePathPosition,
  calculateSpatialPathGains
} = require('./motionEngine.cjs');

const execFileAsync = promisify(execFile);

class WaveEngineController {
  constructor(audioBackend) {
    this.audioBackend = audioBackend;
    this.enabled = false;

    // Phase 7.1 Mixer & Gain Architecture
    this.masterVolume = 100; // 0 to 100
    this.masterMuted = false;
    this.userSpeakerVolumes = new Map(); // sinkId -> number (0..100)
    this.userSpeakerMuted = new Map(); // sinkId -> boolean

    // Movement Mode: 'motion' (Phase 8) | 'spatial' (Phase 7) | 'pattern' (Phase 6)
    this.engineMode = 'motion';

    // Phase 8 Advanced Speaker Motion Engine Parameters
    this.motionMode = 'smooth'; // 'sequential' | 'smooth' | 'circular' | 'bounce' | 'shuffle' | 'spatial_path'
    this.motionSpeed = 0.5; // transitions or cycles per second (0.1 to 5.0)
    this.motionSmoothness = 0.7; // 0.0 to 1.0
    this.motionCurve = 'equal_power'; // 'equal_power' | 'linear' | 'smoothstep'
    this.motionDwellRatio = 0.6; // 0.0 to 0.95 (for sequential dwell hold)
    this.spatialPathType = 'figure8'; // 'waypoints' | 'figure8' | 'lissajous' | 'spiral'
    this.customMotionOrder = null; // string[] or null
    this.shuffleState = { currentIdx: 0, targetIdx: 1, progress: 0.0 };
    this.motionPhase = 0.0;
    this.currentMotionState = null;

    // Phase 6 Wave Engine Parameters
    this.pattern = 'circular'; // 'circular' | 'pingpong' | 'pulse' | 'chase'
    this.speed = 0.5; // cycles per second (0.25 to 2.0)
    this.direction = 'forward'; // 'forward' | 'reverse'
    this.intensity = 1.0; // 0.0 to 1.0
    this.pulseWidth = 2; // 1 to N speakers
    this.chaseFalloff = 0.5; // 0.0 to 1.0
    this.customOrder = null; // string[] or null

    // Phase 7 Spatial Audio Parameters
    this.spatialPattern = 'orbit'; // 'static' | 'orbit' | 'horizontal_sweep' | 'vertical_sweep'
    this.sourceX = 0.0; // Manual / static source X [-1, 1]
    this.sourceY = 0.0; // Manual / static source Y [-1, 1]
    this.currentSourceX = 0.0; // Live animated source X
    this.currentSourceY = 0.0; // Live animated source Y
    this.orbitRadius = 0.7; // [0.05, 1.0]
    this.spatialFalloff = 0.5; // [0.0, 1.0] (0 = Soft, 1 = Focused)
    this.speakerPositions = new Map(); // sinkId (stable identity) -> { x: number, y: number }

    this.branches = new Map(); // sinkId -> BranchState
    this.phase = 0.0; // Wave pattern phase
    this.spatialPhase = 0.0; // Spatial trajectory phase
    this.lastTickTime = 0;
    this.timer = null;
    this.intervalMs = 33; // ~30 Hz tick rate

    this.currentTransition = null;
    this.currentSpatialState = null;
  }

  calculateBranchTargetGain(sinkId, dynamicEngineGain = 1.0) {
    const masterMult = this.masterMuted ? 0.0 : this.masterVolume / 100.0;
    const spkMuted = this.userSpeakerMuted.get(sinkId) ?? false;
    const spkVol = this.userSpeakerVolumes.get(sinkId) ?? 100;
    const spkMult = spkMuted ? 0.0 : spkVol / 100.0;
    const dynGain = typeof dynamicEngineGain === 'number' ? dynamicEngineGain : 1.0;

    const targetGain = masterMult * spkMult * dynGain;
    const targetPercent = Math.max(0, Math.min(100, Math.round(targetGain * 100)));
    return { targetGain, targetPercent };
  }

  async updateBranches(branchList) {
    const existing = this.branches;
    const nextMap = new Map();

    for (const b of branchList) {
      const prev = existing.get(b.sinkId);
      const isNewlyActive = b.state === 'active' && prev?.state !== 'active';
      const isDisconnected = b.state !== 'active';

      const branchState = {
        sinkId: b.sinkId,
        sinkName: b.sinkName,
        branchSinkId: b.branchSinkId,
        sinkInputId: isDisconnected || isNewlyActive ? null : (prev?.sinkInputId || null),
        currentAppliedVolume: isDisconnected || isNewlyActive ? null : (prev?.currentAppliedVolume ?? null),
        pendingVolume: prev?.pendingVolume ?? 100,
        targetGain: prev?.targetGain ?? 1.0,
        isUpdating: false,
        state: b.state
      };
      nextMap.set(b.sinkId, branchState);

      // Preserve user volume & mute state for persistent identity
      if (!this.userSpeakerVolumes.has(b.sinkId)) {
        this.userSpeakerVolumes.set(b.sinkId, 100);
      }
      if (!this.userSpeakerMuted.has(b.sinkId)) {
        this.userSpeakerMuted.set(b.sinkId, false);
      }
    }

    this.branches = nextMap;

    // Ensure all active speakers have assigned 2D coordinates (preserve existing coordinates)
    const activeSinkIds = Array.from(nextMap.values())
      .filter((b) => b.state === 'active')
      .map((b) => b.sinkId);

    const defaultCoords = getDefaultSpeakerPositions(activeSinkIds);
    for (const sinkId of activeSinkIds) {
      if (!this.speakerPositions.has(sinkId)) {
        this.speakerPositions.set(sinkId, defaultCoords[sinkId] || { x: 0.0, y: 0.0 });
      }
    }

    await this.resolveSinkInputIds();
    await this.recalculateAndApplyAllBranches();
  }

  async resolveSinkInputIds() {
    try {
      const sinkInputs = await this.audioBackend.runPactlJson(['--format=json', 'list', 'sink-inputs']);
      for (const branch of this.branches.values()) {
        if (branch.state === 'active') {
          const match = sinkInputs.find(
            (si) =>
              si.properties?.['node.name'] === `${branch.branchSinkId}.playback` ||
              si.properties?.['node.name'] === branch.branchSinkId ||
              si.properties?.['device.description'] === branch.branchSinkId ||
              si.properties?.['node.group'] === branch.branchSinkId
          );
          if (match) {
            if (branch.sinkInputId !== match.index) {
              branch.sinkInputId = match.index;
              branch.currentAppliedVolume = null;
            }
          } else {
            branch.sinkInputId = null;
            branch.currentAppliedVolume = null;
          }
        } else {
          branch.sinkInputId = null;
          branch.currentAppliedVolume = null;
        }
      }
    } catch {
      // Ignored if list fails temporarily
    }
  }

  async setMasterVolume(volume) {
    this.masterVolume = Math.max(0, Math.min(100, Math.round(volume)));
    await this.recalculateAndApplyAllBranches();
  }

  async setMasterMute(muted) {
    this.masterMuted = Boolean(muted);
    await this.recalculateAndApplyAllBranches();
  }

  async setSpeakerVolume(sinkId, volume) {
    this.userSpeakerVolumes.set(sinkId, Math.max(0, Math.min(100, Math.round(volume))));
    await this.recalculateAndApplyBranch(sinkId);
  }

  async setSpeakerMute(sinkId, muted) {
    this.userSpeakerMuted.set(sinkId, Boolean(muted));
    await this.recalculateAndApplyBranch(sinkId);
  }

  async recalculateAndApplyBranch(sinkId) {
    const branch = this.branches.get(sinkId);
    if (!branch || branch.state !== 'active') return;
    await this.applyBranchVolume(branch);
  }

  async recalculateAndApplyAllBranches() {
    await this.resolveSinkInputIds();
    const promises = [];
    for (const branch of this.branches.values()) {
      if (branch.state === 'active') {
        promises.push(this.applyBranchVolume(branch));
      }
    }
    await Promise.all(promises);
  }

  async setConfig(config) {
    // Mode Switcher
    if (config.engineMode === 'motion' || config.engineMode === 'pattern' || config.engineMode === 'spatial') {
      this.engineMode = config.engineMode;
    }

    // Mixer Config
    if (typeof config.masterVolume === 'number') {
      this.masterVolume = Math.max(0, Math.min(100, Math.round(config.masterVolume)));
    }
    if (typeof config.masterMuted === 'boolean') {
      this.masterMuted = config.masterMuted;
    }
    if (config.userSpeakerVolumes && typeof config.userSpeakerVolumes === 'object') {
      for (const [sinkId, vol] of Object.entries(config.userSpeakerVolumes)) {
        if (typeof vol === 'number') {
          this.userSpeakerVolumes.set(sinkId, Math.max(0, Math.min(100, Math.round(vol))));
        }
      }
    }
    if (config.userSpeakerMuted && typeof config.userSpeakerMuted === 'object') {
      for (const [sinkId, muted] of Object.entries(config.userSpeakerMuted)) {
        if (typeof muted === 'boolean') {
          this.userSpeakerMuted.set(sinkId, muted);
        }
      }
    }

    // Phase 8 Advanced Motion Engine Config
    if (typeof config.motionMode === 'string') {
      this.motionMode = normalizeMotionMode(config.motionMode);
    }
    if (typeof config.motionSpeed === 'number' && config.motionSpeed >= 0.1 && config.motionSpeed <= 5.0) {
      this.motionSpeed = config.motionSpeed;
    }
    if (typeof config.motionSmoothness === 'number' && config.motionSmoothness >= 0.0 && config.motionSmoothness <= 1.0) {
      this.motionSmoothness = config.motionSmoothness;
    }
    if (typeof config.motionCurve === 'string') {
      this.motionCurve = normalizeCurveType(config.motionCurve);
    }
    if (typeof config.motionDwellRatio === 'number' && config.motionDwellRatio >= 0.0 && config.motionDwellRatio <= 0.95) {
      this.motionDwellRatio = config.motionDwellRatio;
    }
    const validSpatialPaths = ['waypoints', 'figure8', 'lissajous', 'spiral'];
    if (validSpatialPaths.includes(config.spatialPathType)) {
      this.spatialPathType = config.spatialPathType;
    }
    if (Array.isArray(config.motionOrder)) {
      this.customMotionOrder = config.motionOrder;
    }

    // Phase 6 Wave Engine Config
    if (typeof config.pattern === 'string') {
      this.pattern = normalizePattern(config.pattern);
    }
    if (typeof config.speed === 'number' && config.speed >= 0.1 && config.speed <= 5.0) {
      this.speed = config.speed;
      if (typeof config.motionSpeed !== 'number') this.motionSpeed = config.speed;
    }
    if (config.direction === 'forward' || config.direction === 'reverse') {
      this.direction = config.direction;
    }
    if (typeof config.intensity === 'number' && config.intensity >= 0.0 && config.intensity <= 1.0) {
      this.intensity = config.intensity;
    }
    if (typeof config.pulseWidth === 'number' && config.pulseWidth >= 1) {
      this.pulseWidth = Math.round(config.pulseWidth);
    }
    if (typeof config.chaseFalloff === 'number' && config.chaseFalloff >= 0.0 && config.chaseFalloff <= 1.0) {
      this.chaseFalloff = config.chaseFalloff;
    }
    if (Array.isArray(config.order)) {
      this.customOrder = config.order;
      if (!Array.isArray(config.motionOrder)) this.customMotionOrder = config.order;
    }

    // Phase 7 Spatial Audio Config
    const validSpatialPatterns = ['static', 'orbit', 'horizontal_sweep', 'vertical_sweep'];
    if (validSpatialPatterns.includes(config.spatialPattern)) {
      this.spatialPattern = config.spatialPattern;
    }
    if (typeof config.sourceX === 'number' && !isNaN(config.sourceX)) {
      this.sourceX = Math.max(-1.0, Math.min(1.0, config.sourceX));
      if (this.spatialPattern === 'static') this.currentSourceX = this.sourceX;
    }
    if (typeof config.sourceY === 'number' && !isNaN(config.sourceY)) {
      this.sourceY = Math.max(-1.0, Math.min(1.0, config.sourceY));
      if (this.spatialPattern === 'static') this.currentSourceY = this.sourceY;
    }
    if (typeof config.orbitRadius === 'number' && !isNaN(config.orbitRadius)) {
      this.orbitRadius = Math.max(0.05, Math.min(1.0, config.orbitRadius));
    }
    if (typeof config.spatialFalloff === 'number' && !isNaN(config.spatialFalloff)) {
      this.spatialFalloff = Math.max(0.0, Math.min(1.0, config.spatialFalloff));
    }
    if (config.speakerPositions && typeof config.speakerPositions === 'object') {
      for (const [sinkId, pos] of Object.entries(config.speakerPositions)) {
        if (pos && typeof pos.x === 'number' && typeof pos.y === 'number') {
          this.speakerPositions.set(sinkId, {
            x: Math.max(-1.0, Math.min(1.0, pos.x)),
            y: Math.max(-1.0, Math.min(1.0, pos.y))
          });
        }
      }
    }

    if (typeof config.enabled === 'boolean') {
      if (config.enabled && !this.enabled) {
        this.enabled = true;
        this.phase = 0.0;
        this.spatialPhase = 0.0;
        this.motionPhase = 0.0;
        this.shuffleState = { currentIdx: 0, targetIdx: 1, progress: 0.0 };
        this.lastTickTime = performance.now();
        this.startLoop();
      } else if (!config.enabled && this.enabled) {
        this.enabled = false;
        this.stopLoop();
        await this.recalculateAndApplyAllBranches();
      }
    } else {
      await this.recalculateAndApplyAllBranches();
    }
  }

  startLoop() {
    if (this.timer) clearInterval(this.timer);
    this.lastTickTime = performance.now();
    this.timer = setInterval(() => this.tick(), this.intervalMs);
  }

  stopLoop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
    this.currentTransition = null;
    this.currentSpatialState = null;
    this.currentMotionState = null;
  }

  getActiveOrderedBranches() {
    const activeBranches = Array.from(this.branches.values()).filter((b) => b.state === 'active');
    const orderList = this.engineMode === 'motion' && this.customMotionOrder
      ? this.customMotionOrder
      : this.customOrder;

    if (!orderList || orderList.length === 0) {
      return activeBranches;
    }

    const map = new Map(activeBranches.map((b) => [b.sinkId, b]));
    const ordered = [];
    for (const sinkId of orderList) {
      const b = map.get(sinkId);
      if (b) {
        ordered.push(b);
        map.delete(sinkId);
      }
    }
    for (const b of map.values()) {
      ordered.push(b);
    }
    return ordered;
  }

  tick() {
    if (!this.enabled) return;

    const now = performance.now();
    const dt = Math.max(0, (now - this.lastTickTime) / 1000);
    this.lastTickTime = now;

    const activeList = this.getActiveOrderedBranches();
    const n = activeList.length;

    if (activeList.some((b) => !b.sinkInputId)) {
      this.resolveSinkInputIds().catch(() => {});
    }

    if (n === 0) {
      this.currentTransition = null;
      this.currentSpatialState = null;
      this.currentMotionState = null;
      return;
    }

    if (n === 1) {
      const single = activeList[0];
      single.targetGain = 1.0;
      const { targetPercent } = this.calculateBranchTargetGain(single.sinkId, 1.0);
      single.pendingVolume = targetPercent;
      this.applyBranchVolume(single);

      const singleTransition = {
        pattern: 'single',
        fromSinkId: single.sinkId,
        fromSinkName: single.sinkName,
        fromGain: targetPercent,
        toSinkId: single.sinkId,
        toSinkName: single.sinkName,
        toGain: targetPercent,
        progress: 1.0,
        description: `Single speaker active (${single.sinkName}) — Movement requires ≥ 2 speakers`,
        movementType: 'Single Speaker',
        singleSpeaker: true
      };
      this.currentTransition = singleTransition;

      const spkPos = this.speakerPositions.get(single.sinkId) || { x: 0, y: 0 };
      this.currentSpatialState = {
        source: { x: this.sourceX, y: this.sourceY },
        speakers: [{ sinkId: single.sinkId, sinkName: single.sinkName, x: spkPos.x, y: spkPos.y, distance: 0, gainPercent: targetPercent }],
        closestSpeaker: { sinkId: single.sinkId, sinkName: single.sinkName },
        spatialPattern: this.spatialPattern,
        spatialFalloff: this.spatialFalloff,
        description: `Single speaker active (${single.sinkName})`
      };
      this.currentMotionState = {
        mode: this.motionMode,
        description: `Single speaker active (${single.sinkName})`,
        fromSpeaker: single.sinkName,
        toSpeaker: single.sinkName,
        progress: 1.0
      };
      return;
    }

    // --- MODE 1: ADVANCED MOTION ENGINE MODE (Phase 8) ---
    if (this.engineMode === 'motion') {
      const mode = this.motionMode;
      const speed = this.motionSpeed;
      const intensity = this.intensity;
      const curveType = this.motionCurve;
      const speakers = activeList.map((b) => {
        const pos = this.speakerPositions.get(b.sinkId) || { x: 0.0, y: 0.0 };
        return { sinkId: b.sinkId, sinkName: b.sinkName, x: pos.x, y: pos.y };
      });

      let rawGains = new Array(n).fill(0.0);
      let motionTransition = null;

      if (mode === 'sequential') {
        const delta = speed * dt * n;
        if (this.direction === 'forward') {
          this.motionPhase = (this.motionPhase + delta) % n;
        } else {
          this.motionPhase = (this.motionPhase - delta + n) % n;
        }
        const res = calculateSequentialGains({
          phase: this.motionPhase,
          n,
          dwellRatio: this.motionDwellRatio,
          curveType,
          direction: this.direction,
          speakers
        });
        rawGains = res.gains;
        motionTransition = res.transition;
      } else if (mode === 'smooth') {
        const delta = speed * dt * n;
        if (this.direction === 'forward') {
          this.motionPhase = (this.motionPhase + delta) % n;
        } else {
          this.motionPhase = (this.motionPhase - delta + n) % n;
        }
        const res = calculateSmoothTravelGains({
          phase: this.motionPhase,
          n,
          curveType,
          direction: this.direction,
          speakers
        });
        rawGains = res.gains;
        motionTransition = res.transition;
      } else if (mode === 'circular') {
        const delta = speed * dt * n;
        if (this.direction === 'forward') {
          this.motionPhase = (this.motionPhase + delta) % n;
        } else {
          this.motionPhase = (this.motionPhase - delta + n) % n;
        }
        const res = calculateCircularTravelGains({
          phase: this.motionPhase,
          n,
          curveType,
          direction: this.direction,
          speakers
        });
        rawGains = res.gains;
        motionTransition = res.transition;
      } else if (mode === 'bounce') {
        const cycleLen = 2 * (n - 1);
        const delta = speed * dt * cycleLen;
        if (this.direction === 'forward') {
          this.motionPhase = (this.motionPhase + delta) % cycleLen;
        } else {
          this.motionPhase = (this.motionPhase - delta + cycleLen) % cycleLen;
        }
        const res = calculateBounceGains({
          phase: this.motionPhase,
          n,
          curveType,
          direction: this.direction,
          speakers
        });
        rawGains = res.gains;
        motionTransition = res.transition;
      } else if (mode === 'shuffle') {
        const res = calculateShuffleGains({
          shuffleState: this.shuffleState,
          dt,
          speed,
          curveType,
          speakers
        });
        this.shuffleState = res.nextState;
        rawGains = res.gains;
        motionTransition = res.transition;
      } else if (mode === 'spatial_path') {
        const delta = speed * dt;
        if (this.direction === 'forward') {
          this.motionPhase = (this.motionPhase + delta) % 1.0;
        } else {
          this.motionPhase = (this.motionPhase - delta + 1.0) % 1.0;
        }
        const res = calculateSpatialPathGains({
          pathType: this.spatialPathType,
          pathProgress: this.motionPhase,
          speakers,
          spatialOptions: {
            falloff: this.spatialFalloff,
            radius: this.orbitRadius,
            normalize: true
          }
        });
        rawGains = res.gains;
        motionTransition = res.transition;
        this.currentSourceX = res.source.x;
        this.currentSourceY = res.source.y;
      }

      // Apply centralized intensity scaling and dispatch via authoritative loop
      for (let i = 0; i < n; i++) {
        const branch = activeList[i];
        const rawGain = rawGains[i] ?? 0.0;
        const dynamicGain = (1.0 - intensity) * 1.0 + intensity * rawGain;
        branch.targetGain = dynamicGain;
        this.applyBranchVolume(branch);
      }

      this.currentTransition = motionTransition;
      this.currentMotionState = {
        mode,
        speed,
        direction: this.direction,
        intensity,
        curveType,
        phase: this.motionPhase,
        description: motionTransition?.description || `Motion: ${mode}`,
        fromSinkName: motionTransition?.fromSinkName,
        toSinkName: motionTransition?.toSinkName,
        progress: motionTransition?.progress ?? 0.0,
        sourcePosition: mode === 'spatial_path' ? { x: this.currentSourceX, y: this.currentSourceY } : null
      };

      return;
    }

    // --- MODE 2: SPATIAL POSITION MODE (Phase 7) ---
    if (this.engineMode === 'spatial') {
      // Calculate live sound source trajectory position
      if (this.spatialPattern === 'static') {
        this.currentSourceX = this.sourceX;
        this.currentSourceY = this.sourceY;
      } else {
        // Advance continuous spatial phase: speed cycles/sec
        this.spatialPhase = (this.spatialPhase + this.speed * dt) % 1.0;

        if (this.spatialPattern === 'orbit') {
          const pos = calculateOrbitPosition(this.spatialPhase, this.orbitRadius, { x: 0, y: 0 }, this.direction);
          this.currentSourceX = pos.x;
          this.currentSourceY = pos.y;
        } else if (this.spatialPattern === 'horizontal_sweep') {
          const pos = calculateHorizontalSweepPosition(this.spatialPhase, this.sourceY, this.direction);
          this.currentSourceX = pos.x;
          this.currentSourceY = pos.y;
        } else if (this.spatialPattern === 'vertical_sweep') {
          const pos = calculateVerticalSweepPosition(this.spatialPhase, this.sourceX, this.direction);
          this.currentSourceX = pos.x;
          this.currentSourceY = pos.y;
        }
      }

      // Collect speaker coordinates
      const activeSpeakers = activeList.map((b) => {
        const pos = this.speakerPositions.get(b.sinkId) || { x: 0.0, y: 0.0 };
        return {
          sinkId: b.sinkId,
          sinkName: b.sinkName,
          x: pos.x,
          y: pos.y
        };
      });

      // Calculate spatial gains
      const spatialRes = calculateSpatialGains(
        { x: this.currentSourceX, y: this.currentSourceY },
        activeSpeakers,
        { falloff: this.spatialFalloff, normalize: true }
      );

      // Centralized intensity scaling:
      // dynamicGain = (1.0 - intensity) * 1.0 + intensity * spatialGain
      const intensity = this.intensity;
      for (let i = 0; i < n; i++) {
        const branch = activeList[i];
        const rawGain = spatialRes.gains[i] ?? 0.0;
        const dynamicGain = (1.0 - intensity) * 1.0 + intensity * rawGain;
        branch.targetGain = dynamicGain;
        this.applyBranchVolume(branch);
      }

      this.currentSpatialState = {
        source: { x: this.currentSourceX, y: this.currentSourceY },
        speakers: activeSpeakers.map((s, idx) => ({
          ...s,
          distance: spatialRes.distances[idx],
          rawGain: spatialRes.rawGains[idx],
          gainPercent: activeList[idx].pendingVolume
        })),
        closestSpeaker: spatialRes.closestSpeaker,
        spatialPattern: this.spatialPattern,
        spatialFalloff: this.spatialFalloff,
        speed: this.speed,
        direction: this.direction,
        intensity: this.intensity,
        description: `Source (${this.currentSourceX >= 0 ? '+' : ''}${this.currentSourceX.toFixed(2)}, ${this.currentSourceY >= 0 ? '+' : ''}${this.currentSourceY.toFixed(2)}) → Nearest: ${spatialRes.closestSpeaker?.sinkName || 'Speaker'}`
      };

      // Also provide transition metadata for consistent telemetry
      this.currentTransition = {
        pattern: 'spatial',
        spatialPattern: this.spatialPattern,
        description: this.currentSpatialState.description,
        movementType: `Spatial ${this.spatialPattern.replace('_', ' ').toUpperCase()}`,
        intensity: this.intensity,
        speed: this.speed,
        direction: this.direction
      };

      return;
    }

    // --- MODE 3: PATTERN WAVE MODE (Phase 6) ---
    const pattern = this.pattern;
    const speed = this.speed;

    if (pattern === 'pingpong') {
      const cycleLen = 2 * (n - 1);
      const deltaPhase = speed * dt * cycleLen;
      if (this.direction === 'forward') {
        this.phase = (this.phase + deltaPhase) % cycleLen;
      } else {
        this.phase = (this.phase - deltaPhase + cycleLen) % cycleLen;
      }
    } else {
      const deltaPhase = speed * dt * n;
      if (this.direction === 'forward') {
        this.phase = (this.phase + deltaPhase) % n;
      } else {
        this.phase = (this.phase - deltaPhase + n) % n;
      }
    }

    const { gains: rawGains, transition } = calculatePatternGains({
      pattern: this.pattern,
      phase: this.phase,
      speakerCount: n,
      direction: this.direction,
      pulseWidth: this.pulseWidth,
      chaseFalloff: this.chaseFalloff,
      speakers: activeList.map((b) => ({ sinkId: b.sinkId, sinkName: b.sinkName }))
    });

    const intensity = this.intensity;
    for (let i = 0; i < n; i++) {
      const branch = activeList[i];
      const rawGain = rawGains[i] ?? 0.0;
      const dynamicGain = (1.0 - intensity) * 1.0 + intensity * rawGain;
      branch.targetGain = dynamicGain;
      this.applyBranchVolume(branch);
    }

    if (transition) {
      if (transition.fromSinkId) {
        const fromBranch = this.branches.get(transition.fromSinkId);
        transition.fromGain = fromBranch?.pendingVolume ?? 100;
      }
      if (transition.toSinkId) {
        const toBranch = this.branches.get(transition.toSinkId);
        transition.toGain = toBranch?.pendingVolume ?? 100;
      }
      transition.intensity = intensity;
      transition.speed = speed;
      transition.direction = this.direction;
      this.currentTransition = transition;
    }
  }

  applyBranchVolume(branch) {
    if (!branch || branch.state !== 'active') {
      if (branch) branch.isUpdating = false;
      return Promise.resolve();
    }

    const dynGain = this.enabled ? (branch.targetGain ?? 1.0) : 1.0;
    const { targetPercent } = this.calculateBranchTargetGain(branch.sinkId, dynGain);
    branch.pendingVolume = targetPercent;

    if (!branch.sinkInputId) {
      branch.isUpdating = false;
      this.resolveSinkInputIds().catch(() => {});
      return Promise.resolve();
    }

    if (branch.isUpdating && branch.activePromise) {
      return branch.activePromise;
    }

    branch.isUpdating = true;
    branch.activePromise = (async () => {
      try {
        while (branch.sinkInputId && branch.state === 'active' && branch.pendingVolume !== branch.currentAppliedVolume) {
          const volToSet = branch.pendingVolume;
          await execFileAsync('pactl', ['set-sink-input-volume', String(branch.sinkInputId), `${volToSet}%`]);
          branch.currentAppliedVolume = volToSet;
        }
      } catch (err) {
        branch.currentAppliedVolume = null;
      } finally {
        branch.isUpdating = false;
        branch.activePromise = null;
      }
    })();

    return branch.activePromise;
  }

  async resetAllVolumes(targetPercent = 100) {
    await this.resolveSinkInputIds();
    const promises = [];
    for (const branch of this.branches.values()) {
      branch.targetGain = targetPercent / 100;
      if (branch.sinkInputId && branch.state === 'active') {
        promises.push(this.applyBranchVolume(branch));
      }
    }
    await Promise.all(promises);
  }

  getStatus() {
    const activeList = this.getActiveOrderedBranches();
    const activeCount = activeList.length;
    const effectivePulseWidth = Math.max(1, Math.min(activeCount || 1, this.pulseWidth));

    const speakerPositionsObj = {};
    for (const [sinkId, pos] of this.speakerPositions.entries()) {
      speakerPositionsObj[sinkId] = pos;
    }

    const userSpeakerVolumesObj = {};
    for (const [sinkId, vol] of this.userSpeakerVolumes.entries()) {
      userSpeakerVolumesObj[sinkId] = vol;
    }

    const userSpeakerMutedObj = {};
    for (const [sinkId, muted] of this.userSpeakerMuted.entries()) {
      userSpeakerMutedObj[sinkId] = muted;
    }

    return {
      enabled: this.enabled,
      engineMode: this.engineMode,

      // Master & Per-Speaker Mixer Status (Phase 7.1)
      masterVolume: this.masterVolume,
      masterMuted: this.masterMuted,
      userSpeakerVolumes: userSpeakerVolumesObj,
      userSpeakerMuted: userSpeakerMutedObj,

      // Advanced Motion Engine Status (Phase 8)
      motion: {
        motionMode: this.motionMode,
        motionSpeed: this.motionSpeed,
        motionSmoothness: this.motionSmoothness,
        motionCurve: this.motionCurve,
        motionDwellRatio: this.motionDwellRatio,
        spatialPathType: this.spatialPathType,
        order: activeList.map((b) => b.sinkId),
        activeCount,
        currentMotionState: this.currentMotionState,
        shuffleState: this.shuffleState
      },

      // Wave Pattern Engine Status (Phase 6)
      pattern: this.pattern,
      speed: this.speed,
      direction: this.direction,
      intensity: this.intensity,
      pulseWidth: this.pulseWidth,
      effectivePulseWidth,
      chaseFalloff: this.chaseFalloff,
      order: activeList.map((b) => b.sinkId),
      activeCount,
      currentTransition: this.currentTransition,

      // Spatial Audio Engine Status (Phase 7)
      spatialPattern: this.spatialPattern,
      sourceX: this.sourceX,
      sourceY: this.sourceY,
      currentSourceX: this.currentSourceX,
      currentSourceY: this.currentSourceY,
      orbitRadius: this.orbitRadius,
      spatialFalloff: this.spatialFalloff,
      speakerPositions: speakerPositionsObj,
      currentSpatialState: this.currentSpatialState,

      branchGains: Array.from(this.branches.values()).map((b) => {
        const userVol = this.userSpeakerVolumes.get(b.sinkId) ?? 100;
        const spkMuted = this.userSpeakerMuted.get(b.sinkId) ?? false;
        const dynGain = this.enabled ? (b.targetGain ?? 1.0) : 1.0;
        const dynamicGainPercent = Math.round(dynGain * 100);
        const { targetPercent } = this.calculateBranchTargetGain(b.sinkId, dynGain);
        return {
          sinkId: b.sinkId,
          sinkName: b.sinkName,
          userVolume: userVol,
          muted: spkMuted,
          dynamicGain: dynamicGainPercent,
          gainPercent: b.state === 'active' ? targetPercent : 0,
          appliedVolume: b.state === 'active' ? (b.currentAppliedVolume ?? targetPercent) : 0,
          state: b.state
        };
      })
    };
  }

  async destroy() {
    this.stopLoop();
    this.enabled = false;
    await this.resetAllVolumes(100);
    this.branches.clear();
  }
}

module.exports = { WaveEngineController };
