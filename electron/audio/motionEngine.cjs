/**
 * Advanced Speaker Motion Engine for SpeakerFlow (Phase 8)
 *
 * Provides pure mathematical models for continuous and discrete speaker-to-speaker motion:
 * 1. Sequential Travel (configurable dwell & switch)
 * 2. Smooth Travel (continuous crossfade with equal-power / linear / smoothstep curves)
 * 3. Circular Travel (continuous closed-loop traversal)
 * 4. Bounce (reversible linear motion reflecting at boundaries)
 * 5. Shuffle (controlled non-repeating pseudo-random traversal)
 * 6. Spatial Path (trajectory path generation composed with spatialAudio.cjs distance/gain math)
 *
 * Pure module: Zero pactl, zero subprocesses, zero IPC, zero DOM.
 */

const {
  calculateSpatialGains,
  calculateDistance
} = require('./spatialAudio.cjs');

/**
 * Validates and normalizes motion modes.
 * @param {string} mode
 * @returns {'sequential' | 'smooth' | 'circular' | 'bounce' | 'shuffle' | 'spatial_path'}
 */
function normalizeMotionMode(mode) {
  const valid = ['sequential', 'smooth', 'circular', 'bounce', 'shuffle', 'spatial_path'];
  return valid.includes(mode) ? mode : 'smooth';
}

/**
 * Validates and normalizes crossfade curves.
 * @param {string} curve
 * @returns {'equal_power' | 'linear' | 'smoothstep'}
 */
function normalizeCurveType(curve) {
  const valid = ['equal_power', 'linear', 'smoothstep'];
  return valid.includes(curve) ? curve : 'equal_power';
}

/**
 * Applies crossfade interpolation curve between two weights.
 * @param {number} u - Progress in [0, 1]
 * @param {'equal_power' | 'linear' | 'smoothstep'} [curveType='equal_power']
 * @returns {{ fromGain: number, toGain: number }}
 */
function applyCrossfadeCurve(u, curveType = 'equal_power') {
  const clampedU = Math.max(0.0, Math.min(1.0, u));

  if (curveType === 'linear') {
    return {
      fromGain: 1.0 - clampedU,
      toGain: clampedU
    };
  }

  if (curveType === 'smoothstep') {
    // Hermite interpolation: 3u^2 - 2u^3
    const s = clampedU * clampedU * (3.0 - 2.0 * clampedU);
    return {
      fromGain: 1.0 - s,
      toGain: s
    };
  }

  // Default: Equal-power (sine/cosine law preserves perceived acoustic loudness: g1^2 + g2^2 = 1.0)
  const angle = clampedU * (Math.PI / 2.0);
  return {
    fromGain: Math.cos(angle),
    toGain: Math.sin(angle)
  };
}

/**
 * 1. Sequential Travel
 * Discretely holds audio at speaker k for dwellRatio of the step, then transitions to (k+1).
 *
 * @param {object} params
 * @param {number} params.phase - Normalized position in [0, N)
 * @param {number} params.n - Number of active speakers
 * @param {number} [params.dwellRatio=0.7] - Portion of step held at 100% [0.0, 1.0]
 * @param {'equal_power' | 'linear' | 'smoothstep'} [params.curveType='equal_power']
 * @param {'forward' | 'reverse'} [params.direction='forward']
 * @param {Array<{sinkId: string, sinkName: string}>} [params.speakers]
 */
function calculateSequentialGains({
  phase = 0.0,
  n = 0,
  dwellRatio = 0.7,
  curveType = 'equal_power',
  direction = 'forward',
  speakers = []
}) {
  if (n <= 0) {
    return { gains: [], transition: { mode: 'sequential', description: 'No active speakers' } };
  }
  if (n === 1) {
    const spk = speakers[0] || { sinkId: 'spk_0', sinkName: 'Speaker 1' };
    return {
      gains: [1.0],
      transition: {
        mode: 'sequential',
        fromSinkId: spk.sinkId,
        fromSinkName: spk.sinkName,
        toSinkId: spk.sinkId,
        toSinkName: spk.sinkName,
        progress: 1.0,
        description: `Single speaker active (${spk.sinkName})`,
        singleSpeaker: true
      }
    };
  }

  const normPhase = ((phase % n) + n) % n;
  const k = Math.floor(normPhase);
  const nextK = direction === 'reverse' ? (k - 1 + n) % n : (k + 1) % n;
  const stepFraction = normPhase - k; // [0, 1)

  const clampedDwell = Math.max(0.0, Math.min(0.95, dwellRatio));
  const gains = new Array(n).fill(0.0);

  let fromGain = 1.0;
  let toGain = 0.0;
  let subProgress = 0.0;
  let isDwell = true;

  if (stepFraction <= clampedDwell) {
    // Dwell phase: 100% on current speaker
    gains[k] = 1.0;
    subProgress = clampedDwell > 0 ? stepFraction / clampedDwell : 1.0;
    isDwell = true;
  } else {
    // Transition phase: crossfade to next speaker
    isDwell = false;
    const transSpan = 1.0 - clampedDwell;
    const transProgress = (stepFraction - clampedDwell) / transSpan;
    subProgress = transProgress;
    const cf = applyCrossfadeCurve(transProgress, curveType);
    gains[k] = cf.fromGain;
    gains[nextK] = cf.toGain;
    fromGain = cf.fromGain;
    toGain = cf.toGain;
  }

  const fromSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
  const toSpeaker = speakers[nextK] || { sinkId: `sink_${nextK}`, sinkName: `Speaker ${nextK + 1}` };

  const transition = {
    mode: 'sequential',
    stepIndex: k,
    nextStepIndex: nextK,
    fromSinkId: fromSpeaker.sinkId,
    fromSinkName: fromSpeaker.sinkName,
    toSinkId: toSpeaker.sinkId,
    toSinkName: toSpeaker.sinkName,
    fromGain,
    toGain,
    isDwell,
    stepProgress: stepFraction,
    subProgress,
    description: isDwell
      ? `Holding on ${fromSpeaker.sinkName} (${Math.round((1 - subProgress) * 100)}% dwell left)`
      : `Switching: ${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (${Math.round(subProgress * 100)}%)`,
    movementType: direction === 'reverse' ? 'Sequential Reverse' : 'Sequential Forward'
  };

  return { gains, transition };
}

/**
 * 2. Smooth Travel / Continuous Crossfade
 * Traverses neighboring speakers with continuous equal-power / linear crossfades.
 *
 * @param {object} params
 * @param {number} params.phase - Normalized position in [0, N)
 * @param {number} params.n - Number of active speakers
 * @param {'equal_power' | 'linear' | 'smoothstep'} [params.curveType='equal_power']
 * @param {'forward' | 'reverse'} [params.direction='forward']
 * @param {Array<{sinkId: string, sinkName: string}>} [params.speakers]
 */
function calculateSmoothTravelGains({
  phase = 0.0,
  n = 0,
  curveType = 'equal_power',
  direction = 'forward',
  speakers = []
}) {
  if (n <= 0) {
    return { gains: [], transition: { mode: 'smooth', description: 'No active speakers' } };
  }
  if (n === 1) {
    const spk = speakers[0] || { sinkId: 'spk_0', sinkName: 'Speaker 1' };
    return {
      gains: [1.0],
      transition: {
        mode: 'smooth',
        fromSinkId: spk.sinkId,
        fromSinkName: spk.sinkName,
        toSinkId: spk.sinkId,
        toSinkName: spk.sinkName,
        progress: 1.0,
        description: `Single speaker active (${spk.sinkName})`,
        singleSpeaker: true
      }
    };
  }

  const normPhase = ((phase % n) + n) % n;
  const k = Math.floor(normPhase);
  const nextK = direction === 'reverse' ? (k - 1 + n) % n : (k + 1) % n;
  const u = normPhase - k; // [0, 1)

  const gains = new Array(n).fill(0.0);
  const cf = applyCrossfadeCurve(u, curveType);

  gains[k] = cf.fromGain;
  gains[nextK] = cf.toGain;

  const fromSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
  const toSpeaker = speakers[nextK] || { sinkId: `sink_${nextK}`, sinkName: `Speaker ${nextK + 1}` };

  const transition = {
    mode: 'smooth',
    stepIndex: k,
    nextStepIndex: nextK,
    fromSinkId: fromSpeaker.sinkId,
    fromSinkName: fromSpeaker.sinkName,
    toSinkId: toSpeaker.sinkId,
    toSinkName: toSpeaker.sinkName,
    fromGain: cf.fromGain,
    toGain: cf.toGain,
    progress: u,
    curveType,
    description: `Traveling: ${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (${Math.round(u * 100)}%)`,
    movementType: direction === 'reverse' ? 'Smooth Travel Reverse' : 'Smooth Travel Forward'
  };

  return { gains, transition };
}

/**
 * 3. Circular Travel
 * Alias to smooth continuous loop traversal around ordered speakers.
 */
function calculateCircularTravelGains(params) {
  const res = calculateSmoothTravelGains(params);
  res.transition.mode = 'circular';
  res.transition.movementType = params.direction === 'reverse' ? 'Circular Reverse' : 'Circular Forward';
  return res;
}

/**
 * 4. Bounce Travel (A -> B -> C -> D -> C -> B -> A)
 * Reversible linear motion reflecting at boundaries with cycle length 2 * (N - 1).
 *
 * @param {object} params
 * @param {number} params.phase - Normalized position in [0, 2 * (N - 1))
 * @param {number} params.n - Number of active speakers
 * @param {'equal_power' | 'linear' | 'smoothstep'} [params.curveType='equal_power']
 * @param {'forward' | 'reverse'} [params.direction='forward']
 * @param {Array<{sinkId: string, sinkName: string}>} [params.speakers]
 */
function calculateBounceGains({
  phase = 0.0,
  n = 0,
  curveType = 'equal_power',
  direction = 'forward',
  speakers = []
}) {
  if (n <= 0) {
    return { gains: [], transition: { mode: 'bounce', description: 'No active speakers' } };
  }
  if (n === 1) {
    const spk = speakers[0] || { sinkId: 'spk_0', sinkName: 'Speaker 1' };
    return {
      gains: [1.0],
      transition: {
        mode: 'bounce',
        fromSinkId: spk.sinkId,
        fromSinkName: spk.sinkName,
        toSinkId: spk.sinkId,
        toSinkName: spk.sinkName,
        progress: 1.0,
        description: `Single speaker active (${spk.sinkName})`,
        singleSpeaker: true
      }
    };
  }

  const cycleLen = 2 * (n - 1);
  const normPhase = ((phase % cycleLen) + cycleLen) % cycleLen;

  let x; // Continuous coordinate in [0, n - 1]
  let isMovingForward;

  if (normPhase <= n - 1) {
    x = normPhase;
    isMovingForward = true;
  } else {
    x = cycleLen - normPhase;
    isMovingForward = false;
  }

  const k = Math.min(n - 2, Math.floor(x));
  const u = x - k; // Progress between k and k+1

  const gains = new Array(n).fill(0.0);
  const cf = applyCrossfadeCurve(u, curveType);

  if (x >= n - 1 - 1e-9) {
    gains[n - 1] = 1.0;
  } else {
    gains[k] = cf.fromGain;
    gains[k + 1] = cf.toGain;
  }

  let fromSpeaker;
  let toSpeaker;
  let desc;

  if (isMovingForward) {
    fromSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
    toSpeaker = speakers[k + 1] || { sinkId: `sink_${k + 1}`, sinkName: `Speaker ${k + 2}` };
    desc = `Bouncing: ${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (Outward)`;
  } else {
    fromSpeaker = speakers[k + 1] || { sinkId: `sink_${k + 1}`, sinkName: `Speaker ${k + 2}` };
    toSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
    desc = `Bouncing: ${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (Returning)`;
  }

  const transition = {
    mode: 'bounce',
    fromSinkId: fromSpeaker.sinkId,
    fromSinkName: fromSpeaker.sinkName,
    toSinkId: toSpeaker.sinkId,
    toSinkName: toSpeaker.sinkName,
    fromGain: isMovingForward ? cf.fromGain : cf.toGain,
    toGain: isMovingForward ? cf.toGain : cf.fromGain,
    progress: isMovingForward ? u : 1.0 - u,
    description: desc,
    movementType: isMovingForward ? 'Bounce Outward' : 'Bounce Return',
    bouncingAtEndpoint: x <= 1e-4 || x >= n - 1 - 1e-4
  };

  return { gains, transition };
}

/**
 * 5. Shuffle Travel (Controlled Non-Repeating Random Traversal)
 * State machine managing transition from current speaker to a randomly chosen next speaker.
 *
 * @param {object} params
 * @param {object} params.shuffleState - { currentIdx: number, targetIdx: number, progress: number }
 * @param {number} params.dt - Delta time in seconds
 * @param {number} params.speed - Traversal speed (transitions per second)
 * @param {'equal_power' | 'linear' | 'smoothstep'} [params.curveType='equal_power']
 * @param {Array<{sinkId: string, sinkName: string}>} [params.speakers]
 * @param {Function} [params.randomFn=Math.random] - Optional PRNG for deterministic testing
 */
function calculateShuffleGains({
  shuffleState,
  dt = 0.033,
  speed = 0.5,
  curveType = 'equal_power',
  speakers = [],
  randomFn = Math.random
}) {
  const n = speakers.length;
  if (n <= 0) {
    return {
      gains: [],
      nextState: { currentIdx: 0, targetIdx: 0, progress: 0.0 },
      transition: { mode: 'shuffle', description: 'No active speakers' }
    };
  }

  if (n === 1) {
    const spk = speakers[0] || { sinkId: 'spk_0', sinkName: 'Speaker 1' };
    return {
      gains: [1.0],
      nextState: { currentIdx: 0, targetIdx: 0, progress: 1.0 },
      transition: {
        mode: 'shuffle',
        fromSinkId: spk.sinkId,
        fromSinkName: spk.sinkName,
        toSinkId: spk.sinkId,
        toSinkName: spk.sinkName,
        progress: 1.0,
        description: `Single speaker active (${spk.sinkName})`,
        singleSpeaker: true
      }
    };
  }

  let state = shuffleState ? { ...shuffleState } : { currentIdx: 0, targetIdx: 1, progress: 0.0 };

  // Validate indices
  if (typeof state.currentIdx !== 'number' || state.currentIdx < 0 || state.currentIdx >= n) {
    state.currentIdx = 0;
  }
  if (
    typeof state.targetIdx !== 'number' ||
    state.targetIdx < 0 ||
    state.targetIdx >= n ||
    state.targetIdx === state.currentIdx
  ) {
    // Pick an initial different target
    state.targetIdx = (state.currentIdx + 1) % n;
  }

  // Advance progress
  state.progress = (state.progress || 0.0) + Math.max(0.1, speed) * Math.max(0.0, dt);

  // If transition completed, select next non-repeating target
  while (state.progress >= 1.0) {
    state.currentIdx = state.targetIdx;
    state.progress -= 1.0;

    // Pick new targetIdx !== currentIdx
    const candidates = [];
    for (let i = 0; i < n; i++) {
      if (i !== state.currentIdx) candidates.push(i);
    }
    const rndIdx = Math.floor(randomFn() * candidates.length);
    state.targetIdx = candidates[rndIdx] ?? (state.currentIdx + 1) % n;
  }

  const u = Math.max(0.0, Math.min(1.0, state.progress));
  const cf = applyCrossfadeCurve(u, curveType);

  const gains = new Array(n).fill(0.0);
  gains[state.currentIdx] = cf.fromGain;
  gains[state.targetIdx] = cf.toGain;

  const fromSpeaker = speakers[state.currentIdx] || {
    sinkId: `sink_${state.currentIdx}`,
    sinkName: `Speaker ${state.currentIdx + 1}`
  };
  const toSpeaker = speakers[state.targetIdx] || {
    sinkId: `sink_${state.targetIdx}`,
    sinkName: `Speaker ${state.targetIdx + 1}`
  };

  const transition = {
    mode: 'shuffle',
    fromSinkId: fromSpeaker.sinkId,
    fromSinkName: fromSpeaker.sinkName,
    toSinkId: toSpeaker.sinkId,
    toSinkName: toSpeaker.sinkName,
    fromGain: cf.fromGain,
    toGain: cf.toGain,
    progress: u,
    description: `Shuffling: ${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (${Math.round(u * 100)}%)`,
    movementType: 'Shuffle Random'
  };

  return { gains, nextState: state, transition };
}

/**
 * Calculates continuous 2D coordinate position for various trajectory path types.
 *
 * @param {object} params
 * @param {'waypoints' | 'figure8' | 'lissajous' | 'spiral'} params.pathType
 * @param {number} params.progress - Path phase in [0, 1)
 * @param {Array<{x: number, y: number}>} [params.waypoints] - Ordered waypoints in 2D space
 * @param {object} [params.options]
 * @returns {{ x: number, y: number }}
 */
function calculatePathPosition({
  pathType = 'figure8',
  progress = 0.0,
  waypoints = [],
  options = {}
}) {
  const normProg = ((progress % 1.0) + 1.0) % 1.0;
  const radius = Math.max(0.1, Math.min(1.0, options.radius ?? 0.75));

  // 1. Waypoint interpolation (Linear traversal through speaker points)
  if (pathType === 'waypoints' && Array.isArray(waypoints) && waypoints.length > 0) {
    if (waypoints.length === 1) {
      return { x: waypoints[0].x ?? 0.0, y: waypoints[0].y ?? 0.0 };
    }

    const n = waypoints.length;
    const scaledProg = normProg * n;
    const k = Math.floor(scaledProg) % n;
    const nextK = (k + 1) % n;
    const u = scaledProg - Math.floor(scaledProg);

    // Linear interpolation between waypoint k and k+1
    const p1 = waypoints[k];
    const p2 = waypoints[nextK];

    const x = (p1.x ?? 0) * (1 - u) + (p2.x ?? 0) * u;
    const y = (p1.y ?? 0) * (1 - u) + (p2.y ?? 0) * u;
    return {
      x: Math.max(-1.0, Math.min(1.0, x)),
      y: Math.max(-1.0, Math.min(1.0, y))
    };
  }

  // 2. Figure-8 (Lemniscate of Gerono: x = r * sin(2*pi*t), y = r * sin(4*pi*t)/2)
  if (pathType === 'figure8') {
    const angle = normProg * 2.0 * Math.PI;
    const x = radius * Math.sin(angle);
    const y = (radius * Math.sin(2.0 * angle)) / 1.5;
    return {
      x: Math.max(-1.0, Math.min(1.0, x)),
      y: Math.max(-1.0, Math.min(1.0, y))
    };
  }

  // 3. Lissajous Curve (a=3, b=2 harmonic curve)
  if (pathType === 'lissajous') {
    const a = options.lissajousA ?? 3;
    const b = options.lissajousB ?? 2;
    const delta = options.lissajousDelta ?? Math.PI / 2;
    const angle = normProg * 2.0 * Math.PI;
    const x = radius * Math.sin(a * angle + delta);
    const y = radius * Math.sin(b * angle);
    return {
      x: Math.max(-1.0, Math.min(1.0, x)),
      y: Math.max(-1.0, Math.min(1.0, y))
    };
  }

  // 4. Spiral (expanding and contracting sinusoidal radius)
  if (pathType === 'spiral') {
    const angle = normProg * 4.0 * Math.PI; // 2 revolutions per cycle
    const r = (radius * 0.3) + (radius * 0.7) * (0.5 + 0.5 * Math.sin(normProg * 2.0 * Math.PI));
    const x = r * Math.cos(angle);
    const y = r * Math.sin(angle);
    return {
      x: Math.max(-1.0, Math.min(1.0, x)),
      y: Math.max(-1.0, Math.min(1.0, y))
    };
  }

  // Fallback: Default Circle
  const angle = normProg * 2.0 * Math.PI;
  return {
    x: Math.max(-1.0, Math.min(1.0, radius * Math.cos(angle))),
    y: Math.max(-1.0, Math.min(1.0, radius * Math.sin(angle)))
  };
}

/**
 * 6. Spatial Path Travel
 * Generates virtual source coordinates along a trajectory path and calculates
 * speaker gains by invoking calculateSpatialGains from spatialAudio.cjs (Zero duplication).
 *
 * @param {object} params
 * @param {'waypoints' | 'figure8' | 'lissajous' | 'spiral'} [params.pathType='figure8']
 * @param {number} params.pathProgress - Phase in [0, 1)
 * @param {Array<{sinkId: string, sinkName: string, x: number, y: number}>} [params.speakers]
 * @param {object} [params.spatialOptions] - { falloff: number, normalize: boolean, radius: number }
 */
function calculateSpatialPathGains({
  pathType = 'figure8',
  pathProgress = 0.0,
  speakers = [],
  spatialOptions = {}
}) {
  const n = speakers.length;
  if (n === 0) {
    return {
      gains: [],
      source: { x: 0, y: 0 },
      transition: { mode: 'spatial_path', description: 'No active speakers' }
    };
  }

  // Extract waypoint coordinates from speaker positions
  const waypoints = speakers.map((s) => ({ x: s.x ?? 0.0, y: s.y ?? 0.0 }));

  // Generate source coordinates along trajectory
  const source = calculatePathPosition({
    pathType,
    progress: pathProgress,
    waypoints,
    options: spatialOptions
  });

  // Reuse existing Phase 7 spatial calculation directly
  const spatialRes = calculateSpatialGains(source, speakers, {
    falloff: spatialOptions.falloff ?? 0.5,
    normalize: spatialOptions.normalize !== false
  });

  const transition = {
    mode: 'spatial_path',
    pathType,
    source,
    closestSpeaker: spatialRes.closestSpeaker,
    progress: ((pathProgress % 1.0) + 1.0) % 1.0,
    description: `Trajectory [${pathType.toUpperCase()}]: Source (${source.x >= 0 ? '+' : ''}${source.x.toFixed(2)}, ${source.y >= 0 ? '+' : ''}${source.y.toFixed(2)}) → Nearest: ${spatialRes.closestSpeaker?.sinkName || 'Speaker'}`,
    movementType: `Spatial Path (${pathType})`
  };

  return {
    gains: spatialRes.gains,
    source,
    spatialRes,
    transition
  };
}

module.exports = {
  normalizeMotionMode,
  normalizeCurveType,
  applyCrossfadeCurve,
  calculateSequentialGains,
  calculateSmoothTravelGains,
  calculateCircularTravelGains,
  calculateBounceGains,
  calculateShuffleGains,
  calculatePathPosition,
  calculateSpatialPathGains
};
