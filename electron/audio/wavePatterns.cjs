/**
 * Wave Patterns Engine for SpeakerFlow (Phase 6)
 *
 * Provides pure mathematical models for spatial movement patterns across N ordered speakers.
 * Returns normalized gains in range [0.0, 1.0] and spatial movement metadata.
 * Does NOT execute any pactl commands or interact with audio hardware directly.
 */

/**
 * Validates and clamps pattern parameters.
 * @param {string} pattern
 * @returns {'circular' | 'pingpong' | 'pulse' | 'chase'}
 */
function normalizePattern(pattern) {
  const valid = ['circular', 'pingpong', 'pulse', 'chase'];
  return valid.includes(pattern) ? pattern : 'circular';
}

/**
 * Calculate raw normalized gains for Circular pattern.
 * Crossfades smoothly from speaker k to (k + 1) % N.
 *
 * @param {number} phase - Normalized position in [0, N)
 * @param {number} n - Number of active speakers (>= 2)
 * @param {string} direction - 'forward' | 'reverse'
 * @param {Array<{sinkId: string, sinkName: string}>} speakers
 */
function calculateCircularGains(phase, n, direction, speakers) {
  const normPhase = ((phase % n) + n) % n;
  const k = Math.floor(normPhase);
  const nextK = (k + 1) % n;
  const u = normPhase - k; // [0, 1)

  const gains = new Array(n).fill(0.0);
  gains[k] = 1.0 - u;
  gains[nextK] = u;

  const fromSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
  const toSpeaker = speakers[nextK] || { sinkId: `sink_${nextK}`, sinkName: `Speaker ${nextK + 1}` };

  const transition = {
    pattern: 'circular',
    fromSinkId: fromSpeaker.sinkId,
    fromSinkName: fromSpeaker.sinkName,
    toSinkId: toSpeaker.sinkId,
    toSinkName: toSpeaker.sinkName,
    progress: u,
    description: `${fromSpeaker.sinkName} → ${toSpeaker.sinkName}`,
    movementType: direction === 'reverse' ? 'Circular Reverse' : 'Circular Forward'
  };

  return { gains, transition };
}

/**
 * Calculate raw normalized gains for Ping-Pong pattern.
 * Reflects continuously at boundaries (0 <-> N-1) with cycle length 2 * (N - 1).
 *
 * @param {number} phase - Continuous phase in [0, 2 * (N - 1))
 * @param {number} n - Number of active speakers (>= 2)
 * @param {string} direction - 'forward' | 'reverse'
 * @param {Array<{sinkId: string, sinkName: string}>} speakers
 */
function calculatePingPongGains(phase, n, direction, speakers) {
  const cycleLen = 2 * (n - 1);
  const normPhase = ((phase % cycleLen) + cycleLen) % cycleLen;

  let x; // Spatial coordinate in [0, n - 1]
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

  if (x >= n - 1 - 1e-9) {
    gains[n - 1] = 1.0;
  } else {
    gains[k] = 1.0 - u;
    gains[k + 1] = u;
  }

  let fromSpeaker;
  let toSpeaker;
  let desc;

  if (isMovingForward) {
    fromSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
    toSpeaker = speakers[k + 1] || { sinkId: `sink_${k + 1}`, sinkName: `Speaker ${k + 2}` };
    desc = `${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (Outward)`;
  } else {
    fromSpeaker = speakers[k + 1] || { sinkId: `sink_${k + 1}`, sinkName: `Speaker ${k + 2}` };
    toSpeaker = speakers[k] || { sinkId: `sink_${k}`, sinkName: `Speaker ${k + 1}` };
    desc = `${fromSpeaker.sinkName} → ${toSpeaker.sinkName} (Returning)`;
  }

  const transition = {
    pattern: 'pingpong',
    fromSinkId: fromSpeaker.sinkId,
    fromSinkName: fromSpeaker.sinkName,
    toSinkId: toSpeaker.sinkId,
    toSinkName: toSpeaker.sinkName,
    progress: isMovingForward ? u : 1.0 - u,
    description: desc,
    movementType: isMovingForward ? 'Ping-Pong Forward' : 'Ping-Pong Return',
    bouncingAtEndpoint: x <= 1e-4 || x >= n - 1 - 1e-4
  };

  return { gains, transition };
}

/**
 * Calculate raw normalized gains for Pulse pattern.
 * Moving multi-speaker window of width W.
 *
 * @param {number} phase - Normalized position in [0, N)
 * @param {number} n - Number of active speakers (>= 2)
 * @param {string} direction - 'forward' | 'reverse'
 * @param {number} pulseWidth - Width in speakers (1 to N)
 * @param {Array<{sinkId: string, sinkName: string}>} speakers
 */
function calculatePulseGains(phase, n, direction, pulseWidth, speakers) {
  const clampedWidth = Math.max(1, Math.min(n, Math.round(pulseWidth || 2)));
  const normPhase = ((phase % n) + n) % n;

  const gains = new Array(n).fill(0.0);

  if (clampedWidth >= n) {
    gains.fill(1.0);
    const transition = {
      pattern: 'pulse',
      pulseWidth: clampedWidth,
      fromSinkId: speakers[0]?.sinkId || 'all',
      fromSinkName: 'All Speakers',
      toSinkId: speakers[0]?.sinkId || 'all',
      toSinkName: 'All Speakers',
      progress: 1.0,
      description: `All ${n} Speakers Active (Full Width Pulse)`,
      movementType: 'Pulse Full'
    };
    return { gains, transition };
  }

  const k = Math.floor(normPhase);
  const u = normPhase - k; // [0, 1)

  // Trailing speaker leaving the window fades from 1 -> 0
  gains[k] = 1.0 - u;

  // Middle speakers inside the window remain fully at 1.0
  for (let step = 1; step < clampedWidth; step++) {
    const midIdx = (k + step) % n;
    gains[midIdx] = 1.0;
  }

  // Leading speaker entering the window fades from 0 -> 1
  const leadingIdx = (k + clampedWidth) % n;
  gains[leadingIdx] = u;

  const activeNames = [];
  for (let i = 0; i < n; i++) {
    if (gains[i] > 0.01) {
      activeNames.push(speakers[i]?.sinkName || `Spk ${i + 1}`);
    }
  }

  const transition = {
    pattern: 'pulse',
    pulseWidth: clampedWidth,
    fromSinkId: speakers[k]?.sinkId || `sink_${k}`,
    fromSinkName: speakers[k]?.sinkName || `Speaker ${k + 1}`,
    toSinkId: speakers[leadingIdx]?.sinkId || `sink_${leadingIdx}`,
    toSinkName: speakers[leadingIdx]?.sinkName || `Speaker ${leadingIdx + 1}`,
    progress: u,
    description: `Window [${clampedWidth} speakers]: ${activeNames.join(' + ')}`,
    movementType: `Pulse (Width ${clampedWidth})`
  };

  return { gains, transition };
}

/**
 * Calculate raw normalized gains for Chase pattern.
 * Traveling leader with followers decaying according to chase falloff.
 *
 * @param {number} phase - Normalized position in [0, N)
 * @param {number} n - Number of active speakers (>= 2)
 * @param {string} direction - 'forward' | 'reverse'
 * @param {number} chaseFalloff - Falloff factor [0.0, 1.0] (0 = broad/even, 1 = focused leader)
 * @param {Array<{sinkId: string, sinkName: string}>} speakers
 */
function calculateChaseGains(phase, n, direction, chaseFalloff, speakers) {
  const normPhase = ((phase % n) + n) % n;
  const f = Math.max(0.0, Math.min(1.0, typeof chaseFalloff === 'number' ? chaseFalloff : 0.5));

  // Decay factor alpha: f=0.0 -> 0.85 (broad), f=0.5 -> 0.55 (moderate), f=1.0 -> 0.15 (sharp focus)
  const alpha = 0.85 - 0.70 * f;

  // Base gains at integer offsets from leader
  const baseGains = new Array(n);
  for (let j = 0; j < n; j++) {
    baseGains[j] = Math.pow(alpha, j);
  }

  // Compute interpolated gain for each speaker based on its circular distance from continuous phase
  const gains = new Array(n);
  let maxGainIdx = 0;
  let maxGain = -1;

  for (let i = 0; i < n; i++) {
    const dist = ((i - normPhase) % n + n) % n; // [0, n)
    const j = Math.floor(dist);
    const v = dist - j; // [0, 1)
    const nextJ = (j + 1) % n;

    // Smooth linear interpolation between offset steps
    const interpolatedGain = (1.0 - v) * baseGains[j] + v * baseGains[nextJ];
    gains[i] = Math.max(0.0, Math.min(1.0, interpolatedGain));

    if (gains[i] > maxGain) {
      maxGain = gains[i];
      maxGainIdx = i;
    }
  }

  const leaderSpeaker = speakers[maxGainIdx] || { sinkId: `sink_${maxGainIdx}`, sinkName: `Speaker ${maxGainIdx + 1}` };
  const nextLeaderIdx = (maxGainIdx + 1) % n;
  const nextLeader = speakers[nextLeaderIdx] || { sinkId: `sink_${nextLeaderIdx}`, sinkName: `Speaker ${nextLeaderIdx + 1}` };

  const transition = {
    pattern: 'chase',
    chaseFalloff: f,
    leaderSinkId: leaderSpeaker.sinkId,
    leaderSinkName: leaderSpeaker.sinkName,
    fromSinkId: leaderSpeaker.sinkId,
    fromSinkName: leaderSpeaker.sinkName,
    toSinkId: nextLeader.sinkId,
    toSinkName: nextLeader.sinkName,
    progress: normPhase - Math.floor(normPhase),
    description: `Leader: ${leaderSpeaker.sinkName} (${Math.round(maxGain * 100)}%) → ${nextLeader.sinkName}`,
    movementType: `Chase (Falloff ${Math.round(f * 100)}%)`
  };

  return { gains, transition };
}

/**
 * Main pattern calculation dispatcher.
 * Returns raw normalized gains [0.0, 1.0] for N speakers.
 *
 * @param {object} params
 * @param {string} params.pattern - 'circular' | 'pingpong' | 'pulse' | 'chase'
 * @param {number} params.phase - Continuous position / phase
 * @param {number} params.speakerCount - N (>= 2)
 * @param {string} params.direction - 'forward' | 'reverse'
 * @param {number} [params.pulseWidth] - Configured pulse width (1..N)
 * @param {number} [params.chaseFalloff] - Configured chase falloff (0..1)
 * @param {Array<{sinkId: string, sinkName: string}>} [params.speakers]
 * @returns {{gains: number[], transition: object}}
 */
function calculatePatternGains({
  pattern = 'circular',
  phase = 0.0,
  speakerCount = 2,
  direction = 'forward',
  pulseWidth = 2,
  chaseFalloff = 0.5,
  speakers = []
}) {
  const n = Math.max(1, speakerCount);

  if (n === 1) {
    const singleSpeaker = speakers[0] || { sinkId: 'single', sinkName: 'Speaker 1' };
    return {
      gains: [1.0],
      transition: {
        pattern: 'single',
        fromSinkId: singleSpeaker.sinkId,
        fromSinkName: singleSpeaker.sinkName,
        toSinkId: singleSpeaker.sinkId,
        toSinkName: singleSpeaker.sinkName,
        progress: 1.0,
        description: `Single speaker active (${singleSpeaker.sinkName}) — Wave movement requires ≥ 2 speakers`,
        movementType: 'Single Speaker',
        singleSpeaker: true
      }
    };
  }

  const p = normalizePattern(pattern);

  switch (p) {
    case 'pingpong':
      return calculatePingPongGains(phase, n, direction, speakers);
    case 'pulse':
      return calculatePulseGains(phase, n, direction, pulseWidth, speakers);
    case 'chase':
      return calculateChaseGains(phase, n, direction, chaseFalloff, speakers);
    case 'circular':
    default:
      return calculateCircularGains(phase, n, direction, speakers);
  }
}

module.exports = {
  normalizePattern,
  calculatePatternGains,
  calculateCircularGains,
  calculatePingPongGains,
  calculatePulseGains,
  calculateChaseGains
};
