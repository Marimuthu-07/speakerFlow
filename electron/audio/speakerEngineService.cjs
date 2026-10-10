const { randomUUID } = require('node:crypto');
const { createAudioPipelineAdapter } = require('./createAudioBackend.cjs');
const { WaveEngineController } = require('./waveEngineController.cjs');
const { DeviceDelayStore } = require('./deviceDelayStore.cjs');

function isVirtualSink(sink) {
  if (!sink) return false;
  if (sink.isVirtual || sink.properties?.['node.virtual'] === 'true') return true;
  if (sink.id && (sink.id.startsWith('speakerflow.session.') || sink.id === 'all_speakers')) return true;
  if (sink.name && (sink.name.startsWith('speakerflow.session.') || sink.name === 'all_speakers')) return true;
  if (/voicemeeter|vb-audio|virtual|cable|steam streaming/i.test(sink.name || '') || /voicemeeter|vb-audio|virtual|cable|steam streaming/i.test(sink.description || '')) return true;
  return false;
}

function isTelephonyOrSecondaryEndpoint(sink) {
  if (!sink) return false;

  // 0. Explicit communications role from backend
  if (sink.isCommunications) {
    return true;
  }

  // 1. Form-factor check if provided by backend metadata
  if (sink.formFactor && typeof sink.formFactor === 'string') {
    const ff = sink.formFactor.toLowerCase();
    if (ff.includes('handset') || ff.includes('telephony')) {
      return true;
    }
  }

  // 2. Role / communications check if provided by backend metadata
  if (sink.role && typeof sink.role === 'string') {
    const r = sink.role.toLowerCase();
    if (r.includes('communications') || r.includes('telephony')) {
      return true;
    }
  }

  // 3. Friendly name, description, technical name, and ID check
  const text = `${sink.name || ''} ${sink.description || ''} ${sink.technicalName || ''} ${sink.id || ''}`.toLowerCase();
  if (
    text.includes('hands-free') ||
    text.includes('handsfree') ||
    text.includes('hfp') ||
    text.includes('telephony') ||
    text.includes('communications') ||
    text.includes('bthfpenum') ||
    text.includes('bthhfenum')
  ) {
    return true;
  }

  return false;
}

class SpeakerEngineService {
  constructor(audioBackend, pipelineAdapter, delayStore = null) {
    this.audioBackend = audioBackend;
    this.pipelineAdapter = pipelineAdapter || createAudioPipelineAdapter(audioBackend);
    this.delayStore = delayStore || new DeviceDelayStore();
    this.session = null;
    this.lastMessage = '';
    this.waveController = new WaveEngineController(audioBackend);
    this.reconnectTimers = new Map(); // sinkId -> timeoutHandle
    this.onMasterVolumeChanged = null;
    this.onSessionChanged = null;
    this._isReconciling = false;
    this._reconcilePending = false;
  }

  getDeviceDelay(deviceId) {
    return this.delayStore.getDelay(deviceId);
  }

  supportsAcousticDelay() {
    return process.platform === 'win32' || Boolean(this.pipelineAdapter?.setBranchDelay);
  }

  async getStatus() {
    await this.reconcileSession();
    const allOutputs = await this.audioBackend.listOutputDevicesWithStatus();
    const outputs = allOutputs.filter((device) => !isVirtualSink(device)).map((device) => ({
      ...device,
      configuredDelayMs: this.delayStore.getDelay(device.id)
    }));

    const waveStatus = this.waveController.getStatus();

    return {
      outputs,
      supportsAcousticDelay: this.supportsAcousticDelay(),
      session: this.session
        ? {
            active: true,
            state: this.session.state,
            streamId: this.session.streamId,
            streamName: this.session.streamName,
            streamActive: this.session.streamActive !== false,
            isManualStreamSelection: Boolean(this.session.isManualStreamSelection),
            originalSinkId: this.session.originalSinkId,
            originalSinkName: this.session.originalSinkName,
            previousDefaultSinkId: this.session.previousDefaultSinkId,
            defaultOutputEstablished: this.session.defaultOutputEstablished,
            ingressSinkId: this.session.ingressSinkId,
            ingressStatus: this.session.ingressStatus,
            tapSourceId: this.session.tapSourceId,
            tapStatus: this.session.tapStatus,
            captureMode: this.session.captureMode || (this.session.pipeline?.captureMode || (this.session.pipeline?.isNativeProcessLoopback ? 'process' : 'endpoint')),
            isNativeProcessLoopback: Boolean(this.session.isNativeProcessLoopback || this.session.pipeline?.isNativeProcessLoopback),
            branches: this.session.branches.map((b) => ({
              sinkId: b.sinkId,
              sinkName: b.sinkName,
              branchSinkId: b.branchSinkId,
              state: b.state,
              error: b.error || null,
              delayMs: b.delayMs ?? 0,
              configuredDelayMs: b.delayMs ?? this.delayStore.getDelay(b.sinkId),
              effectiveDelayFrames: b.delayFrames ?? (b.handle?.delayFrames ?? 0),
              intentionalDisconnect: b.intentionalDisconnect || false,
              waitingForDevice: Boolean(b.waitingForDevice || b.state === 'waiting_device'),
              lifecycleState: b.lifecycleStateName || (b.state === 'active' ? 'Running' : b.state === 'waiting_device' ? 'WaitingForDevice' : b.state),
              lifecycleStateName: b.lifecycleStateName || (b.state === 'active' ? 'Running' : b.state === 'waiting_device' ? 'WaitingForDevice' : b.state),
              renderState: b.renderState || (b.state === 'active' ? 'Running' : 'Preroll'),
              framesRendered: b.framesRendered ?? 0,
              silentFramesRendered: b.silentFramesRendered ?? 0,
              deviceState: b.deviceState ?? 0,
              lastRecoveryTimestamp: b.lastRecoveryTimestamp ?? 0
            })),
            activeBranchCount: this.session.branches.filter((b) => b.state === 'active').length,
            totalBranchCount: this.session.branches.length,
            message: this.lastMessage,
            wave: waveStatus
          }
        : { active: false, state: 'idle', message: this.lastMessage, wave: waveStatus }
    };
  }

  async startSession(streamId, selectedSinkIds, isManual = false) {
    if (this.session) {
      throw new Error('A Speaker Engine session is already active.');
    }

    this.clearAllReconnectTimers();

    const sinkIdList = Array.isArray(selectedSinkIds)
      ? selectedSinkIds
      : [selectedSinkIds].filter(Boolean);

    if (sinkIdList.length === 0) {
      throw new Error('At least one physical output must be selected to start the engine.');
    }

    const [stream, allSinks, previousDefaultSinkId] = await Promise.all([
      this.findApplicationStream(streamId),
      this.audioBackend.listSinks(),
      this.audioBackend.getDefaultOutputId().catch(() => null)
    ]);

    if (!stream) {
      throw new Error('The selected application stream is no longer available.');
    }

    const validPhysicalSinks = [];
    for (const sinkId of sinkIdList) {
      const sink = allSinks.find((item) => item.id === sinkId || item.name === sinkId);
      if (sink && !isVirtualSink(sink)) {
        validPhysicalSinks.push({
          id: sink.id || sink.name,
          name: sink.name || sink.description || sink.id
        });
      }
    }

    if (validPhysicalSinks.length === 0) {
      throw new Error('None of the selected outputs are currently available physical sinks.');
    }

    const allPhysicalSinksAtStartup = allSinks.filter((sink) => !isVirtualSink(sink));

    const sessionId = randomUUID();
    const ingressSinkId = `speakerflow.session.${sessionId}.ingress`;
    const tapSourceId = `speakerflow.session.${sessionId}.tap`;

    const session = {
      sessionId,
      pipeline: null,
      ingressSinkId,
      tapSourceId,
      ingressStatus: 'starting',
      tapStatus: 'starting',
      state: 'starting',
      streamId: stream.id,
      streamProcessId: stream.processId,
      streamName: stream.streamName ? `${stream.applicationName} — ${stream.streamName}` : stream.applicationName,
      streamActive: stream.isActive,
      isManualStreamSelection: Boolean(isManual),
      originalSinkId: stream.currentSinkId,
      originalSinkName: stream.currentSinkName,
      previousDefaultSinkId,
      defaultOutputEstablished: false,
      branches: [],
      branchSequence: validPhysicalSinks.length,
      selectedSinkIds: new Set(validPhysicalSinks.map((s) => s.id)),
      initialAvailableSinkIds: new Set(allPhysicalSinksAtStartup.map((s) => s.name)),
      stopping: false
    };
    this.session = session;
    this.lastMessage = '';

    try {
      const pipeline = await this.pipelineAdapter.createPipeline({
        sessionId,
        ingressSinkId,
        tapSourceId,
        stream,
        streamId: stream.id,
        selectedPhysicalSinks: validPhysicalSinks,
        onIngressError: (error) => {
          if (this.session === session) {
            session.ingressStatus = 'error';
            session.tapStatus = 'error';
            this.lastMessage = `The temporary Speaker Engine ingress could not start: ${error.message}`;
            this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
          }
        },
        onIngressExit: (reason) => {
          if (this.session === session && !session.stopping) {
            session.ingressStatus = 'stopped';
            session.tapStatus = 'stopped';
            this.lastMessage = `The temporary Speaker Engine ingress stopped unexpectedly (${reason}).`;
            this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
          }
        }
      });

      session.pipeline = pipeline;
      session.captureMode = pipeline.captureMode || (pipeline.isNativeProcessLoopback ? 'process' : 'endpoint');
      session.isNativeProcessLoopback = Boolean(pipeline.isNativeProcessLoopback);
      session.ingressStatus = 'active';
      session.tapStatus = 'active';
      const effectiveIngressSinkId = pipeline.ingressSinkId || ingressSinkId;
      session.ingressSinkId = effectiveIngressSinkId;

      if (process.platform === 'win32') {
        if (typeof pipeline.setMasterVolume === 'function') {
          pipeline.setMasterVolume((this.waveController.masterVolume ?? 100) / 100);
        }
        if (typeof pipeline.setMasterMute === 'function') {
          pipeline.setMasterMute(Boolean(this.waveController.masterMuted));
        }
      }

      let successfulBranches = 0;
      for (let i = 0; i < validPhysicalSinks.length; i++) {
        const physicalSink = validPhysicalSinks[i];
        const branchSinkId = `speakerflow.session.${sessionId}.branch.${i}`;
        const initialDelay = this.delayStore.getDelay(physicalSink.id);
        const branchObj = {
          sinkId: physicalSink.id,
          sinkName: physicalSink.name,
          branchSinkId,
          handle: null,
          state: 'starting',
          error: null,
          delayMs: initialDelay,
          delayFrames: 0,
          intentionalDisconnect: false,
          retryCount: 0,
          reconnecting: false
        };
        session.branches.push(branchObj);

        try {
          const branchHandle = await pipeline.createBranch({
            branchSinkId,
            physicalSinkId: physicalSink.id,
            delayMs: initialDelay,
            onError: (error) => {
              branchObj.state = 'failed';
              branchObj.error = error.message;
              if (this.session === session && !session.stopping) {
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
              }
            },
            onDisconnect: (reason) => {
              if (this.session === session && !session.stopping && branchObj.state === 'active') {
                branchObj.state = 'disconnected';
                branchObj.handle = null;
                this.lastMessage = `Branch for "${physicalSink.name}" stopped (${reason}).`;
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
                if (!branchObj.intentionalDisconnect) {
                  this.scheduleAutoReconnect(branchObj.sinkId);
                }
              }
            }
          });
          branchObj.handle = branchHandle;
          branchObj.delayMs = branchHandle?.delayMs ?? branchObj.delayMs;
          branchObj.delayFrames = branchHandle?.delayFrames ?? 0;
          branchObj.state = 'active';

          if (process.platform === 'win32') {
            const spkVol = this.waveController.userSpeakerVolumes.get(physicalSink.id) ?? 100;
            const spkMute = this.waveController.userSpeakerMuted.get(physicalSink.id) ?? false;
            if (typeof pipeline.setBranchVolume === 'function') {
              pipeline.setBranchVolume(branchSinkId, spkVol / 100);
            }
            if (typeof pipeline.setBranchMute === 'function') {
              pipeline.setBranchMute(branchSinkId, spkMute);
            }
          }

          successfulBranches++;
        } catch (branchError) {
          branchObj.state = 'failed';
          branchObj.error = branchError.message;
        }
      }

      if (successfulBranches === 0) {
        throw new Error('All selected output branches failed to start.');
      }

      if (!pipeline.isNativeProcessLoopback) {
        await this.audioBackend.moveSinkInput(stream.id, effectiveIngressSinkId);
        await this.verifyStreamSink(stream.id, effectiveIngressSinkId);

        // On non-Windows platforms (e.g. Linux PipeWire), establish the temporary ingress sink as system default
        if (process.platform !== 'win32') {
          await this.audioBackend.setDefaultOutput(effectiveIngressSinkId);
          await this.verifyDefaultOutput(effectiveIngressSinkId);
          session.defaultOutputEstablished = true;
        }
      }

      session.state = 'running';

      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);

      // Perform final reconciliation pass in case device or stream events occurred during startup
      await this.reconcileSession().catch(() => {});

      const modeText = pipeline.isNativeProcessLoopback ? ' (direct process capture)' : '';
      this.lastMessage = `Audio routed across ${successfulBranches} physical output branch${successfulBranches === 1 ? '' : 'es'}${modeText}.`;
    } catch (error) {
      await this.stopSession({ restore: true, preserveMessage: true });
      throw error;
    }
  }

  async stopSession(options = {}) {
    const session = this.session;
    this.clearAllReconnectTimers();

    if (!session) return;

    session.stopping = true;
    let restoreError = null;

    await this.waveController.destroy().catch(() => {});

    if (options.restore !== false) {
      // 1. Restore previous system default output BEFORE destroying temporary ingress sink (non-Windows)
      if (process.platform !== 'win32' && session.previousDefaultSinkId && session.defaultOutputEstablished) {
        try {
          await this.audioBackend.setDefaultOutput(session.previousDefaultSinkId);
          await this.verifyDefaultOutput(session.previousDefaultSinkId);
          session.defaultOutputEstablished = false;
        } catch (error) {
          if (!restoreError) restoreError = error;
        }
      }

      // 2. Restore application stream to original output
      if (!session.pipeline?.isNativeProcessLoopback && !session.isNativeProcessLoopback) {
        try {
          const stream = await this.findApplicationStream(session.streamId);
          if (stream?.currentSinkId === session.ingressSinkId) {
            if (process.platform === 'win32') {
              if (session.originalSinkId && session.originalSinkId !== session.previousDefaultSinkId) {
                await this.audioBackend.moveSinkInput(session.streamId, session.originalSinkId);
              } else {
                await this.audioBackend.clearApplicationOutput(session.streamId);
              }
            } else {
              const originalSink = await this.audioBackend.findSinkByName(session.originalSinkId);
              if (!originalSink) {
                throw new Error(`The original output, ${session.originalSinkName}, is unavailable.`);
              }
              await this.audioBackend.moveSinkInput(session.streamId, originalSink.id);
              await this.verifyStreamSink(session.streamId, originalSink.id);
            }
          }
        } catch (error) {
          if (!restoreError) restoreError = error;
        }
      }
    }

    if (session.pipeline) {
      await session.pipeline.destroy().catch(() => {});
    }

    if (this.session === session) {
      this.session = null;
    }

    if (typeof this.onSessionChanged === 'function') {
      try {
        this.onSessionChanged();
      } catch (err) {
        console.warn('[SpeakerEngineService] onSessionChanged error on stopSession:', err);
      }
    }

    if (restoreError) {
      this.lastMessage = `Temporary engine objects were removed, but the original output could not be restored: ${restoreError.message}`;
      throw restoreError;
    }

    if (!options.preserveMessage) {
      this.lastMessage = (session.pipeline?.isNativeProcessLoopback || session.isNativeProcessLoopback)
        ? 'The Multi-Speaker Engine session stopped.'
        : 'The temporary Multi-Speaker Engine session stopped and the original output was restored.';
    }
  }

  async getWaveStatus() {
    return this.waveController.getStatus();
  }

  async setWaveConfig(config) {
    await this.waveController.setConfig(config);
    return this.waveController.getStatus();
  }

  async setMotionConfig(config) {
    await this.waveController.setConfig({ ...config, engineMode: config.engineMode || 'motion' });
    return this.waveController.getStatus();
  }

  async setMasterVolume(volume) {
    const clamped = Math.max(0, Math.min(100, Math.round(volume)));
    await this.waveController.setMasterVolume(clamped);
    if (process.platform === 'win32') {
      if (this.session?.pipeline && typeof this.session.pipeline.setMasterVolume === 'function') {
        this.session.pipeline.setMasterVolume(clamped / 100);
      }
    } else {
      if (this.session && this.session.ingressSinkId) {
        await this.audioBackend.setSinkVolume(this.session.ingressSinkId, clamped).catch(() => {});
      }
    }
    return this.waveController.getStatus();
  }

  async setMasterMute(muted) {
    const isMuted = Boolean(muted);
    await this.waveController.setMasterMute(isMuted);
    if (process.platform === 'win32') {
      if (this.session?.pipeline && typeof this.session.pipeline.setMasterMute === 'function') {
        this.session.pipeline.setMasterMute(isMuted);
      }
    } else {
      if (this.session && this.session.ingressSinkId) {
        await this.audioBackend.setSinkMute(this.session.ingressSinkId, isMuted).catch(() => {});
      }
    }
    return this.waveController.getStatus();
  }

  async setSpeakerVolume(sinkId, volume) {
    await this.waveController.setSpeakerVolume(sinkId, volume);
    if (process.platform === 'win32' && this.session?.pipeline && typeof this.session.pipeline.setBranchVolume === 'function') {
      const branch = this.session.branches.find((b) => b.sinkId === sinkId || b.branchSinkId === sinkId);
      if (branch) {
        this.session.pipeline.setBranchVolume(branch.branchSinkId, Math.max(0, Math.min(100, volume)) / 100);
      }
    }
    return this.waveController.getStatus();
  }

  async setSpeakerMute(sinkId, muted) {
    await this.waveController.setSpeakerMute(sinkId, muted);
    if (process.platform === 'win32' && this.session?.pipeline && typeof this.session.pipeline.setBranchMute === 'function') {
      const branch = this.session.branches.find((b) => b.sinkId === sinkId || b.branchSinkId === sinkId);
      if (branch) {
        this.session.pipeline.setBranchMute(branch.branchSinkId, Boolean(muted));
      }
    }
    return this.waveController.getStatus();
  }

  async setSessionStream(streamId, isManual = true) {
    const session = this.session;
    if (!session || session.stopping) {
      throw new Error('No active Speaker Engine session.');
    }

    const stream = await this.findApplicationStream(streamId);
    if (!stream) {
      throw new Error(`The selected application stream ${streamId} was not found.`);
    }

    if (!session.pipeline?.isNativeProcessLoopback && !session.isNativeProcessLoopback) {
      // Virtual-cable / endpoint loopback mode
      if (stream.currentSinkId !== session.ingressSinkId) {
        await this.audioBackend.moveSinkInput(stream.id, session.ingressSinkId);
        await this.verifyStreamSink(stream.id, session.ingressSinkId).catch(() => {});
      }

      session.isManualStreamSelection = Boolean(isManual);
      session.streamId = stream.id;
      session.streamProcessId = stream.processId;
      session.streamName = stream.streamName
        ? `${stream.applicationName} — ${stream.streamName}`
        : stream.applicationName;
      session.streamActive = stream.isActive;
      this.lastMessage = `Switched audio routing to ${session.streamName}.`;
    } else {
      // Native process capture mode: strictly resolve target process ID
      const targetPid = (typeof stream.processId === 'number' && Number.isInteger(stream.processId) && stream.processId > 0)
        ? stream.processId
        : (Number.isInteger(Number(streamId)) && Number(streamId) > 0 ? Number(streamId) : null);

      if (!targetPid) {
        const err = new Error(
          `Cannot switch native process capture to "${stream.applicationName}": no valid process ID found.`
        );
        this.lastMessage = err.message;
        throw err;
      }

      if (session.pipeline && typeof session.pipeline.switchProcess === 'function') {
        try {
          await session.pipeline.switchProcess(targetPid);

          // Synchronize session branch objects with pipeline branches after reconstruction
          if (session.pipeline.branches) {
            for (const branchObj of session.branches) {
              const handle = session.pipeline.branches.get(branchObj.branchSinkId);
              if (handle) {
                branchObj.handle = handle;
                branchObj.delayMs = handle.delayMs ?? branchObj.delayMs;
                branchObj.delayFrames = handle.delayFrames ?? 0;
                branchObj.state = 'active';
                branchObj.error = null;
              } else {
                branchObj.handle = null;
                branchObj.state = 'failed';
                branchObj.error = 'Branch reconstruction failed after process switch.';
              }
            }
          }

          // Successfully switched native process capture! Now update session stream metadata
          session.isManualStreamSelection = Boolean(isManual);
          session.streamId = stream.id;
          session.streamProcessId = stream.processId;
          session.streamName = stream.streamName
            ? `${stream.applicationName} — ${stream.streamName}`
            : stream.applicationName;
          session.streamActive = stream.isActive;

          this.reconcileSessionState();
          await this.waveController.updateBranches(session.branches).catch(() => {});
          this.lastMessage = `Switched audio capture to ${session.streamName}.`;
        } catch (switchErr) {
          // Native switching failed: do NOT mark new stream as active.
          // Synchronize session branches to reflect any lost native branches.
          if (session.pipeline?.branches) {
            for (const branchObj of session.branches) {
              if (!session.pipeline.branches.has(branchObj.branchSinkId)) {
                branchObj.handle = null;
                branchObj.state = 'failed';
                branchObj.error = 'Render branch lost during process retargeting failure.';
              }
            }
          }
          this.reconcileSessionState();
          this.lastMessage = `Failed to switch capture to ${stream.applicationName}: ${switchErr.message}`;
          if (typeof this.onSessionChanged === 'function') {
            this.onSessionChanged();
          }
          throw switchErr;
        }
      }
    }

    if (typeof this.onSessionChanged === 'function') {
      this.onSessionChanged();
    }

    return this.getStatus();
  }

  async disconnectBranch(sinkId) {
    const session = this.session;
    if (!session || session.stopping) {
      throw new Error('No active Speaker Engine session.');
    }

    const branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch) {
      throw new Error(`Branch for output ${sinkId} was not found in active session.`);
    }

    branch.intentionalDisconnect = true;
    this.clearReconnectTimer(sinkId);

    if (branch.state !== 'disconnected') {
      branch.state = 'disconnected';
      if (branch.handle && session.pipeline) {
        await session.pipeline.destroyBranch(branch.handle);
        branch.handle = null;
      }
      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);
      this.lastMessage = `Output "${branch.sinkName}" disconnected from session.`;
    }
  }

  async reconnectBranch(sinkId) {
    const session = this.session;
    if (!session || session.stopping) {
      throw new Error('No active Speaker Engine session.');
    }

    let branch = session.branches.find((b) => b.sinkId === sinkId);
    if (branch) {
      branch.intentionalDisconnect = false;
      branch.retryCount = 0;
      this.clearReconnectTimer(sinkId);
    }

    await this._performBranchReconnect(sinkId, false);
  }

  async setSpeakerDelay(sinkId, delayMs) {
    if (!sinkId || typeof sinkId !== 'string') {
      throw new Error('A valid speaker sink ID is required.');
    }

    const val = Number(delayMs);
    if (Number.isNaN(val) || !Number.isFinite(val) || val < 0 || val > 500) {
      throw new Error('Speaker delay must be a finite number between 0 and 500 milliseconds.');
    }

    const session = this.session;
    const branch = session?.branches?.find((b) => b.sinkId === sinkId || b.branchSinkId === sinkId);

    let effectiveFrames = 0;
    if (session && !session.stopping && branch) {
      if (session.pipeline && typeof session.pipeline.setBranchDelay === 'function') {
        const res = await session.pipeline.setBranchDelay(branch.branchSinkId, val);
        if (res && res.delayFrames !== undefined) {
          branch.delayFrames = res.delayFrames;
          effectiveFrames = res.delayFrames;
        }
      }
      branch.delayMs = val;
    }

    const physicalSinkId = branch ? branch.sinkId : sinkId;
    this.delayStore.setDelay(physicalSinkId, val);

    if (typeof this.onSessionChanged === 'function') {
      this.onSessionChanged();
    }

    return {
      success: true,
      sinkId: physicalSinkId,
      branchSinkId: branch?.branchSinkId || null,
      configuredDelayMs: val,
      effectiveDelayFrames: effectiveFrames
    };
  }

  scheduleAutoReconnect(sinkId, delayMs = 1500) {
    const session = this.session;
    if (!session || session.stopping) return;

    const branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch || branch.intentionalDisconnect || branch.reconnecting) return;

    this.clearReconnectTimer(sinkId);

    const timer = setTimeout(async () => {
      this.reconnectTimers.delete(sinkId);
      if (this.session !== session || session.stopping || branch.intentionalDisconnect || branch.state === 'active') {
        return;
      }
      await this._performBranchReconnect(sinkId, true);
    }, delayMs);

    this.reconnectTimers.set(sinkId, timer);
  }

  clearReconnectTimer(sinkId) {
    const timer = this.reconnectTimers.get(sinkId);
    if (timer) {
      clearTimeout(timer);
      this.reconnectTimers.delete(sinkId);
    }
  }

  clearAllReconnectTimers() {
    for (const timer of this.reconnectTimers.values()) {
      clearTimeout(timer);
    }
    this.reconnectTimers.clear();
    if (this._settlingTimer) {
      clearTimeout(this._settlingTimer);
      this._settlingTimer = null;
    }
  }

  scheduleSettlingReconciliation(delayMs = 4500) {
    if (this._settlingTimer) {
      clearTimeout(this._settlingTimer);
    }
    this._settlingTimer = setTimeout(() => {
      this._settlingTimer = null;
      if (this.session && !this.session.stopping) {
        this.reconcileSession().catch(() => {});
      }
    }, delayMs);
  }

  async _performBranchReconnect(sinkId, isAuto = false) {
    const session = this.session;
    if (!session || session.stopping) return;

    const branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch) return;
    if (branch.reconnecting) return;
    branch.reconnecting = true;

    try {
      if (process.platform === 'win32' && session.pipeline && typeof session.pipeline.triggerBranchRecovery === 'function') {
        const engineStatus = typeof session.pipeline.getEngineStatus === 'function' ? session.pipeline.getEngineStatus() : null;
        const nativeBranch = engineStatus?.branches?.find(
          (b) => b.branchId === branch.branchSinkId || b.deviceId === branch.sinkId
        );
        if (nativeBranch || (!engineStatus && session.pipeline)) {
          // Native engine already manages this branch slot: wake recovery coordinator.
          // Manual user action ("Connect to Engine") uses forceImmediate = true (!isAuto).
          // Automatic scheduled reconnect uses forceImmediate = false.
          session.pipeline.triggerBranchRecovery(branch.branchSinkId, !isAuto);
          branch.state = 'reconnecting';
          branch.waitingForDevice = true;
          branch.reconnecting = false;
          this.reconcileSessionState();
          this.scheduleSettlingReconciliation(4500);
          return;
        }
      }

      const allSinks = await this.audioBackend.listSinks();
      const physicalSink = allSinks.find((s) => s.id === sinkId || s.name === sinkId);

      if (!physicalSink || isVirtualSink(physicalSink)) {
        branch.reconnecting = false;
        if (!isAuto) {
          throw new Error('The physical output is not currently available.');
        }
        branch.state = 'waiting_device';
        branch.waitingForDevice = true;
        branch.error = null;
        branch.retryCount = (branch.retryCount || 0) + 1;
        const nextDelay = Math.min(5000, Math.round(1500 * Math.pow(1.5, Math.min(branch.retryCount, 3))));
        this.scheduleAutoReconnect(sinkId, nextDelay);
        return;
      }

      if (branch.handle && session.pipeline) {
        await session.pipeline.destroyBranch(branch.handle).catch(() => {});
        branch.handle = null;
      }

      if (session.pipeline) {
        const initialDelay = branch.delayMs ?? this.delayStore.getDelay(branch.sinkId);
        const branchHandle = await session.pipeline.createBranch({
          branchSinkId: branch.branchSinkId,
          physicalSinkId: branch.sinkId,
          delayMs: initialDelay,
          onError: (error) => {
            branch.state = 'failed';
            branch.error = error.message;
            if (this.session === session && !session.stopping) {
              this.reconcileSessionState();
              this.waveController.updateBranches(session.branches).catch(() => {});
            }
          },
          onDisconnect: (reason) => {
            if (this.session === session && !session.stopping && branch.state === 'active') {
              branch.state = 'disconnected';
              branch.handle = null;
              this.lastMessage = `Branch for "${branch.sinkName}" stopped (${reason}).`;
              this.reconcileSessionState();
              this.waveController.updateBranches(session.branches).catch(() => {});
              if (!branch.intentionalDisconnect) {
                this.scheduleAutoReconnect(branch.sinkId);
              }
            }
          }
        });

        branch.handle = branchHandle;
        branch.delayMs = branchHandle?.delayMs ?? initialDelay;
        branch.delayFrames = branchHandle?.delayFrames ?? (branch.delayFrames ?? 0);
        branch.state = 'active';
        branch.error = null;
        branch.retryCount = 0;
        this.clearReconnectTimer(sinkId);
        this.reconcileSessionState();
        await this.waveController.updateBranches(session.branches);
        this.lastMessage = isAuto
          ? `Output "${branch.sinkName}" reconnected automatically.`
          : `Output "${branch.sinkName}" reconnected to session.`;
      }
    } catch (error) {
      branch.reconnecting = false;
      branch.state = 'disconnected';
      branch.error = error.message;
      if (isAuto) {
        branch.retryCount = (branch.retryCount || 0) + 1;
        const nextDelay = Math.min(5000, Math.round(1500 * Math.pow(1.5, Math.min(branch.retryCount, 3))));
        this.scheduleAutoReconnect(sinkId, nextDelay);
      } else {
        throw error;
      }
    } finally {
      branch.reconnecting = false;
    }
  }

  async reassertTargetIngress(force = true) {
    const session = this.session;
    if (!session || session.stopping || session.state === 'starting') return false;
    if (session.pipeline?.isNativeProcessLoopback || session.isNativeProcessLoopback) return false;

    const targetPid = session.streamProcessId;
    if (!targetPid || targetPid === process.pid) return false;

    try {
      const streams = await this.audioBackend.listApplicationStreams();
      const streamList = streams || [];
      const matchingStreams = streamList.filter((s) => s.processId === targetPid);
      if (matchingStreams.length === 0) return false;

      let rerouted = false;
      for (const s of matchingStreams) {
        const appLower = (s.applicationName || '').toLowerCase();
        if (appLower.includes('electron') || appLower.includes('speakerflow') ||
            appLower.includes('antigravity') || appLower.includes('voicemeeter')) {
          continue;
        }

        if (s.currentSinkId && s.currentSinkId !== session.ingressSinkId) {
          const now = Date.now();
          if (force || !session.lastRerouteTimestamp || (now - session.lastRerouteTimestamp) > 1000) {
            session.lastRerouteTimestamp = now;
            await this.audioBackend.moveSinkInput(targetPid, session.ingressSinkId).catch(() => {});
            await this.verifyStreamSink(targetPid, session.ingressSinkId, 500).catch(() => {});
            rerouted = true;
          }
          break;
        }
      }
      return rerouted;
    } catch {
      return false;
    }
  }

  async reconcileSession(options = {}) {
    const session = this.session;
    if (!session || session.stopping || session.state === 'starting') return;

    if (options && options.forceIngressReassert) {
      this._forceIngressPending = true;
    }

    if (this._isReconciling) {
      this._reconcilePending = true;
      return;
    }

    this._isReconciling = true;
    try {
      do {
        this._reconcilePending = false;
        const forceIngress = Boolean(this._forceIngressPending);
        this._forceIngressPending = false;
        await this._performReconciliation(forceIngress);
      } while (this._reconcilePending && this.session && !this.session.stopping && this.session.state !== 'starting');
    } finally {
      this._isReconciling = false;
      this._reconcilePending = false;
      this._forceIngressPending = false;
    }
  }

  async _performReconciliation(forceIngressReassert = false) {
    const session = this.session;
    if (!session || session.stopping || session.state === 'starting') return;

    const sinks = await this.audioBackend.listSinks();
    const validPhysicalSinks = sinks.filter((sink) => !isVirtualSink(sink));
    const validPhysicalMap = new Map();
    for (const s of validPhysicalSinks) {
      if (s.name) validPhysicalMap.set(s.name, s);
      if (s.id) validPhysicalMap.set(s.id, s);
    }

    // 1. Observe active session ingress sink volume and mute (OS / pavucontrol / Media key master control on Linux)
    const ingressSink = sinks.find((s) => s.id === session.ingressSinkId || s.name === session.ingressSinkId);
    if (ingressSink && process.platform !== 'win32') {
      const sinkVol = parseSinkVolumePercent(ingressSink.volumePercent !== undefined ? ingressSink.volumePercent : ingressSink.volume);
      const sinkMuted = Boolean(ingressSink.mute);
      const currentWave = this.waveController.getStatus();

      let masterChanged = false;
      if (currentWave.masterVolume !== sinkVol) {
        await this.waveController.setMasterVolume(sinkVol);
        masterChanged = true;
      }
      if (currentWave.masterMuted !== sinkMuted) {
        await this.waveController.setMasterMute(sinkMuted);
        masterChanged = true;
      }

      if (masterChanged && typeof this.onMasterVolumeChanged === 'function') {
        this.onMasterVolumeChanged({ masterVolume: sinkVol, masterMuted: sinkMuted });
      }
    }

    // 2. Reconcile active application stream for session (Auto vs Manual mode)
    let streamChanged = false;
    try {
      const streams = await this.audioBackend.listApplicationStreams();
      const streamList = streams || [];
      const streamMap = new Map(streamList.map((s) => [s.id, s]));
      const currentStream = session.streamId ? streamMap.get(session.streamId) : null;

      let targetStream = null;

      if (session.isManualStreamSelection) {
        if (currentStream) {
          // Manual mode: keep user's stream locked while it exists
          targetStream = currentStream;
        } else {
          // Manually selected stream disappeared: reset manual mode and fallback
          session.isManualStreamSelection = false;
          targetStream = streamList.find((s) => s.isActive) || streamList[0] || null;
        }
      } else {
        // Auto mode:
        if (currentStream && currentStream.isActive) {
          // Current stream is active -> maintain stability (no jumping)
          targetStream = currentStream;
        } else {
          // Current stream is inactive/paused or missing -> prefer active playing stream
          const activeStream = streamList.find((s) => s.isActive);
          if (activeStream) {
            targetStream = activeStream;
          } else if (currentStream) {
            // If no stream is active, stay on current paused stream
            targetStream = currentStream;
          } else {
            // Fallback to first stream or null
            targetStream = streamList[0] || null;
          }
        }
      }

      if (targetStream) {
        if (session.streamId !== targetStream.id) {
          session.streamId = targetStream.id;
          streamChanged = true;
          if (!session.pipeline?.isNativeProcessLoopback && !session.isNativeProcessLoopback) {
            // Direct newly targeted stream to engine ingress if not already there
            if (targetStream.currentSinkId !== session.ingressSinkId) {
              await this.audioBackend.moveSinkInput(targetStream.id, session.ingressSinkId).catch(() => {});
            }
          }
        }

        // Phase 2F.1-F: Target Application Ingress Re-Assertion (Prevent Chrome escaping to physical Bluetooth)
        if (!session.pipeline?.isNativeProcessLoopback && !session.isNativeProcessLoopback) {
          const targetPid = session.streamProcessId || targetStream.processId;
          if (targetPid && targetPid !== process.pid) {
            const matchingStreams = streamList.filter((s) => s.processId === targetPid);
            for (const s of matchingStreams) {
              const appLower = (s.applicationName || '').toLowerCase();
              if (appLower.includes('electron') || appLower.includes('speakerflow') ||
                  appLower.includes('antigravity') || appLower.includes('voicemeeter')) {
                continue;
              }

              if (s.currentSinkId && s.currentSinkId !== session.ingressSinkId) {
                const now = Date.now();
                if (forceIngressReassert || !session.lastRerouteTimestamp || (now - session.lastRerouteTimestamp) > 1000) {
                  session.lastRerouteTimestamp = now;
                  await this.audioBackend.moveSinkInput(targetPid, session.ingressSinkId).catch(() => {});
                  await this.verifyStreamSink(targetPid, session.ingressSinkId, 500).catch(() => {});
                }
                break;
              }
            }
          }
        }

        const freshName = targetStream.streamName
          ? `${targetStream.applicationName} — ${targetStream.streamName}`
          : targetStream.applicationName;
        const freshActive = targetStream.isActive;

        if (session.streamName !== freshName || session.streamActive !== freshActive) {
          session.streamName = freshName;
          session.streamActive = freshActive;
          streamChanged = true;
        }
      } else {
        if (session.streamActive !== false) {
          session.streamActive = false;
          streamChanged = true;
        }
      }
    } catch {}

    let branchesChanged = false;

    // 2.5 Sync native engine branch lifecycle on Windows
    if (process.platform === 'win32' && session.pipeline && typeof session.pipeline.getEngineStatus === 'function') {
      const engineStatus = session.pipeline.getEngineStatus();
      if (engineStatus) {
        if (!engineStatus.isCapturing && session.state === 'running' && !session.stopping) {
          const reason = engineStatus.lastError || 'WASAPI capture stopped';
          session.ingressStatus = 'stopped';
          session.tapStatus = 'stopped';
          this.lastMessage = (session.pipeline.isNativeProcessLoopback || session.isNativeProcessLoopback)
            ? `Target application capture ended: ${reason}`
            : `Capture stopped unexpectedly: ${reason}`;
          await this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
          return;
        }

        if (Array.isArray(engineStatus.branches)) {
        const nativeMap = new Map();
        for (const nb of engineStatus.branches) {
          if (nb.branchId) nativeMap.set(nb.branchId, nb);
          if (nb.deviceId) nativeMap.set(nb.deviceId, nb);
        }

        for (const branch of session.branches) {
          const nb = nativeMap.get(branch.branchSinkId) || nativeMap.get(branch.sinkId);
          if (nb) {
            branch.lifecycleStateName = nb.lifecycleStateName;
            branch.renderState = nb.renderState;
            branch.framesRendered = nb.framesRendered;
            branch.silentFramesRendered = nb.silentFramesRendered;
            branch.deviceState = nb.deviceState;
            branch.lastRecoveryTimestamp = nb.lastRecoveryTimestamp;

            // FIX 3: Check OS physical device presence first!
            // Priority:
            // 1. Physical endpoint presence
            // 2. Recovery/waiting state
            // 3. Native render lifecycle
            // 4. Active state
            const isPhysicallyPresent = validPhysicalMap.has(branch.sinkId);
            if (!isPhysicallyPresent && !branch.intentionalDisconnect) {
              branch.waitingForDevice = true;
              if (branch.state !== 'waiting_device' && branch.state !== 'reconnecting') {
                branch.state = 'waiting_device';
                branch.error = null;
                branchesChanged = true;
              }
              continue;
            }

            if (nb.lifecycleStateName === 'Running') {
              if (branch.state !== 'active') {
                branch.state = 'active';
                branch.error = null;
                branch.waitingForDevice = false;
                branch.retryCount = 0;
                branch.reconnecting = false;
                branchesChanged = true;
                if (process.platform === 'win32' && session.pipeline) {
                  const spkVol = this.waveController.userSpeakerVolumes.get(branch.sinkId) ?? 100;
                  const spkMute = this.waveController.userSpeakerMuted.get(branch.sinkId) ?? false;
                  if (typeof session.pipeline.setBranchVolume === 'function') {
                    session.pipeline.setBranchVolume(branch.branchSinkId, spkVol / 100);
                  }
                  if (typeof session.pipeline.setBranchMute === 'function') {
                    session.pipeline.setBranchMute(branch.branchSinkId, spkMute);
                  }
                }
              }
            } else if (nb.lifecycleStateName === 'WaitingForDevice' || nb.waitingForDevice) {
              if (branch.state !== 'waiting_device') {
                branch.state = 'waiting_device';
                branch.error = null;
                branch.waitingForDevice = true;
                branchesChanged = true;
              }
              this.scheduleSettlingReconciliation(2000);
            } else if (
              nb.lifecycleStateName === 'RecoveryPending' ||
              nb.lifecycleStateName === 'Reinitializing' ||
              nb.lifecycleStateName === 'Starting'
            ) {
              if (branch.state !== 'reconnecting') {
                branch.state = 'reconnecting';
                branch.waitingForDevice = true;
                branchesChanged = true;
              }
              this.scheduleSettlingReconciliation(4500);
            } else if (nb.lifecycleStateName === 'DeviceLost') {
              if (branch.state !== 'waiting_device') {
                branch.state = 'waiting_device';
                branch.waitingForDevice = true;
                branchesChanged = true;
              }
            } else if (nb.lifecycleStateName === 'Failed') {
              if (branch.state !== 'failed') {
                branch.state = 'failed';
                branch.error = nb.lastError || 'Endpoint initialization failed.';
                branchesChanged = true;
              }
            }
          }
        }
      }
    }
  }

    // 3. Clean up branches for physical sinks that no longer exist in the system
    for (let i = session.branches.length - 1; i >= 0; i--) {
      const branch = session.branches[i];
      const isPresent = validPhysicalMap.has(branch.sinkId);

      if (!isPresent) {
        if (process.platform === 'win32' && !branch.intentionalDisconnect) {
          // On Windows, the native engine maintains persistent recovery for disconnected outputs.
          // Do NOT remove or destroy the branch during temporary device disappearance!
          branch.waitingForDevice = true;
          if (branch.state !== 'waiting_device' && branch.state !== 'reconnecting') {
            branch.state = 'waiting_device';
            branch.error = null;
            branchesChanged = true;
          }
          continue;
        }

        if (branch.state === 'active') {
          branch.state = 'disconnected';
          branch.error = 'Physical sink disconnected.';
          if (branch.handle && session.pipeline) {
            await session.pipeline.destroyBranch(branch.handle).catch(() => {});
            branch.handle = null;
          }
          this.lastMessage = `Output "${branch.sinkName}" disconnected.`;
          branchesChanged = true;
        }
        this.clearReconnectTimer(branch.sinkId);
        if (!branch.intentionalDisconnect) {
          session.branches.splice(i, 1);
          branchesChanged = true;
        }
      } else if (branch.waitingForDevice && !branch.intentionalDisconnect) {
        // Physical endpoint returned! Wake up native recovery coordinator
        // Automatic reconciliation passes forceImmediate = false to preserve Bluetooth settling deadline
        if (process.platform === 'win32' && session.pipeline && typeof session.pipeline.triggerBranchRecovery === 'function') {
          session.pipeline.triggerBranchRecovery(branch.branchSinkId, false);
        }
        if (!session.pipeline || typeof session.pipeline.getEngineStatus !== 'function') {
          branch.state = 'active';
          branch.waitingForDevice = false;
          branchesChanged = true;
        }
      } else if (branch.state === 'disconnected' && !branch.intentionalDisconnect && !branch.reconnecting) {
        if (process.platform === 'win32' && session.pipeline && typeof session.pipeline.triggerBranchRecovery === 'function') {
          session.pipeline.triggerBranchRecovery(branch.branchSinkId, false);
        } else {
          this.scheduleAutoReconnect(branch.sinkId, 100);
        }
      }
    }

    // 4. Hot-plug newly connected eligible physical outputs into the running session
    const existingSinkIds = new Set(session.branches.map((b) => b.sinkId));
    for (const physicalSink of validPhysicalSinks) {
      if (existingSinkIds.has(physicalSink.name) || existingSinkIds.has(physicalSink.id)) continue;

      // Filter out telephony/hands-free communication profiles from automatic branch addition
      if (isTelephonyOrSecondaryEndpoint(physicalSink)) {
        continue;
      }

      // Target selection semantics:
      // Must be explicitly selected by the user OR be a brand new device plugged in after session started
      const isSelected =
        session.selectedSinkIds &&
        (session.selectedSinkIds.has(physicalSink.name) || session.selectedSinkIds.has(physicalSink.id));
      const isNewlyConnectedDevice =
        session.initialAvailableSinkIds &&
        !session.initialAvailableSinkIds.has(physicalSink.name) &&
        !session.initialAvailableSinkIds.has(physicalSink.id);

      if (!isSelected && !isNewlyConnectedDevice) {
        // Device was available at startup but intentionally excluded by the user -> do not attach
        continue;
      }

      if (physicalSink.name) existingSinkIds.add(physicalSink.name);
      if (physicalSink.id) existingSinkIds.add(physicalSink.id);
      if (session.selectedSinkIds) {
        if (physicalSink.name) session.selectedSinkIds.add(physicalSink.name);
        if (physicalSink.id) session.selectedSinkIds.add(physicalSink.id);
      }

      const branchIdx = typeof session.branchSequence === 'number' ? session.branchSequence++ : session.branches.length;
      const branchSinkId = `speakerflow.session.${session.sessionId}.branch.${branchIdx}`;
      const branchObj = {
        sinkId: physicalSink.id || physicalSink.name,
        sinkName: physicalSink.name || physicalSink.description || physicalSink.id,
        branchSinkId,
        handle: null,
        state: 'starting',
        error: null,
        intentionalDisconnect: false,
        retryCount: 0,
        reconnecting: false
      };
      session.branches.push(branchObj);

      if (session.pipeline) {
        try {
          const branchHandle = await session.pipeline.createBranch({
            branchSinkId,
            physicalSinkId: physicalSink.id || physicalSink.name,
            onError: (error) => {
              branchObj.state = 'failed';
              branchObj.error = error.message;
              if (this.session === session && !session.stopping) {
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
              }
            },
            onDisconnect: (reason) => {
              if (this.session === session && !session.stopping && branchObj.state === 'active') {
                branchObj.state = 'disconnected';
                branchObj.handle = null;
                this.lastMessage = `Branch for "${branchObj.sinkName}" stopped (${reason}).`;
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
                if (!branchObj.intentionalDisconnect) {
                  this.scheduleAutoReconnect(branchObj.sinkId);
                }
              }
            }
          });
          branchObj.handle = branchHandle;
          branchObj.state = 'active';

          if (process.platform === 'win32') {
            const spkVol = this.waveController.userSpeakerVolumes.get(physicalSink.id) ?? 100;
            const spkMute = this.waveController.userSpeakerMuted.get(physicalSink.id) ?? false;
            if (typeof session.pipeline.setBranchVolume === 'function') {
              session.pipeline.setBranchVolume(branchSinkId, spkVol / 100);
            }
            if (typeof session.pipeline.setBranchMute === 'function') {
              session.pipeline.setBranchMute(branchSinkId, spkMute);
            }
          }

          this.lastMessage = `New output "${branchObj.sinkName}" connected and added to engine session.`;
          branchesChanged = true;
        } catch (branchErr) {
          branchObj.state = 'failed';
          branchObj.error = branchErr.message;
          branchesChanged = true;
        }
      }
    }

    if (branchesChanged) {
      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);
    }

    if ((branchesChanged || streamChanged) && typeof this.onSessionChanged === 'function') {
      this.onSessionChanged();
    }
  }

  reconcileSessionState() {
    const session = this.session;
    if (!session) return;

    const activeCount = session.branches.filter((b) => b.state === 'active').length;
    if (activeCount === 0) {
      session.state = 'degraded';
      this.lastMessage = 'No selected physical outputs are currently available.';
    } else if (activeCount < session.branches.length) {
      session.state = 'degraded';
    } else {
      session.state = 'running';
    }
  }

  async findApplicationStream(streamId) {
    const streams = await this.audioBackend.listApplicationStreams();
    let match = streams.find(
      (stream) =>
        stream.id === streamId ||
        String(stream.id) === String(streamId) ||
        (typeof stream.id === 'number' && stream.id === Number(streamId))
    );
    if (!match && this.session && this.session.streamProcessId) {
      match = streams.find((s) => s.processId === this.session.streamProcessId);
    }
    if (!match && streamId) {
      match = streams.find((s) => s.processId && String(streamId).includes(String(s.processId)));
    }
    if (match && this.session && this.session.streamProcessId === match.processId && this.session.streamId !== match.id) {
      this.session.streamId = match.id;
    }
    return match || null;
  }

  async verifyStreamSink(streamId, expectedSinkId, timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const stream = await this.findApplicationStream(streamId);
      if (stream && stream.currentSinkId === expectedSinkId) {
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    const stream = await this.findApplicationStream(streamId);
    if (!stream) {
      throw new Error('The application stream disappeared before routing could be verified.');
    }
    throw new Error(`The application stream did not reach the expected temporary audio path (current: ${stream.currentSinkId}, expected: ${expectedSinkId}).`);
  }

  async verifyDefaultOutput(expectedSinkId, timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      try {
        const currentDefault = await this.audioBackend.getDefaultOutputId();
        if (currentDefault === expectedSinkId) {
          return;
        }
      } catch {}
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    const currentDefault = await this.audioBackend.getDefaultOutputId().catch(() => 'unknown');
    if (currentDefault !== expectedSinkId) {
      throw new Error(`The system default output did not switch to the expected output (current: ${currentDefault}, expected: ${expectedSinkId}).`);
    }
  }
}

function parseSinkVolumePercent(volumeObj) {
  if (typeof volumeObj === 'number') {
    return Math.max(0, Math.min(100, Math.round(volumeObj)));
  }
  if (!volumeObj || typeof volumeObj !== 'object') return 100;
  const channels = Object.values(volumeObj);
  if (channels.length === 0) return 100;
  let totalPercent = 0;
  let count = 0;
  for (const ch of channels) {
    if (ch && typeof ch.value_percent === 'string') {
      totalPercent += parseInt(ch.value_percent.replace('%', ''), 10) || 0;
      count++;
    } else if (ch && typeof ch.value === 'number') {
      totalPercent += Math.round((ch.value / 65536) * 100);
      count++;
    }
  }
  return count > 0 ? Math.round(totalPercent / count) : 100;
}

module.exports = { SpeakerEngineService, isTelephonyOrSecondaryEndpoint };
