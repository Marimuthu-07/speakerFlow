/**
 * Spatial Audio Engine for SpeakerFlow (Phase 7)
 *
 * Provides pure 2D coordinate geometry, distance calculations, falloff attenuation models,
 * gain normalization, and continuous spatial trajectory calculators (Orbit, Sweeps, Static).
 *
 * Coordinates:
 *   x in [-1, +1]: -1 = Left,   0 = Center, +1 = Right
 *   y in [-1, +1]: -1 = Back,   0 = Center, +1 = Front
 *
 * Pure module: Zero pactl, zero subprocesses, zero IPC, zero DOM.
 */

/**
 * Calculates Euclidean distance between two 2D points.
 * @param {number} x1
 * @param {number} y1
 * @param {number} x2
 * @param {number} y2
 * @returns {number}
 */
function calculateDistance(x1, y1, x2, y2) {
  const dx = (x1 ?? 0) - (x2 ?? 0);
  const dy = (y1 ?? 0) - (y2 ?? 0);
  return Math.sqrt(dx * dx + dy * dy);
}

/**
 * Calculates raw distance attenuation gain for a given distance and falloff factor.
 * Uses a continuous power-law model:
 *   gain = 1 / (1 + (distance / d0)^p)
 * where p scales from 1.0 (Soft) to 4.0 (Focused).
 *
 * Output is guaranteed to be in (0.0, 1.0], stable at distance=0 (yields 1.0), and never NaN/Infinity.
 *
 * @param {number} distance - Non-negative distance
 * @param {number} [falloff=0.5] - Falloff factor in [0.0, 1.0] (0 = Soft, 1 = Focused)
 * @returns {number} Raw gain in (0.0, 1.0]
 */
function calculateSpatialAttenuation(distance, falloff = 0.5) {
  const d = Math.max(0, typeof distance === 'number' && !isNaN(distance) ? distance : 0);
  const f = Math.max(0.0, Math.min(1.0, typeof falloff === 'number' && !isNaN(falloff) ? falloff : 0.5));

  // Power exponent: 1.0 (Soft) to 4.0 (Focused)
  const power = 1.0 + 3.0 * f;
  // Base distance radius scaling
  const d0 = 0.8 + 0.4 * (1.0 - f);

  const normalizedDist = d / d0;
  const rawGain = 1.0 / (1.0 + Math.pow(normalizedDist, power));

  return Math.max(0.0, Math.min(1.0, rawGain));
}

/**
 * Normalizes an array of raw gains so the maximum gain reaches 1.0.
 * Preserves relative ratios between speakers.
 *
 * @param {number[]} rawGains
 * @returns {number[]}
 */
function normalizeSpatialGains(rawGains) {
  if (!Array.isArray(rawGains) || rawGains.length === 0) return [];
  if (rawGains.length === 1) return [1.0];

  let maxGain = -1;
  for (const g of rawGains) {
    if (g > maxGain) maxGain = g;
  }

  if (maxGain <= 1e-9) {
    return new Array(rawGains.length).fill(1.0);
  }

  return rawGains.map((g) => Math.max(0.0, Math.min(1.0, g / maxGain)));
}

/**
 * Calculates normalized spatial gains for N speakers given a sound source position.
 *
 * @param {object} source - { x: number, y: number }
 * @param {Array<{sinkId: string, sinkName: string, x: number, y: number}>} speakers
 * @param {object} [options]
 * @param {number} [options.falloff=0.5] - Spatial falloff [0.0, 1.0]
 * @param {boolean} [options.normalize=true] - Whether to normalize closest speaker to 1.0
 * @returns {{
 *   gains: number[],
 *   distances: number[],
 *   closestSpeaker: object|null,
 *   rawGains: number[],
 *   metadata: object
 * }}
 */
function calculateSpatialGains(source, speakers = [], options = {}) {
  const n = speakers.length;
  const falloff = options.falloff ?? 0.5;
  const shouldNormalize = options.normalize !== false;

  const srcX = Math.max(-1.0, Math.min(1.0, source?.x ?? 0.0));
  const srcY = Math.max(-1.0, Math.min(1.0, source?.y ?? 0.0));

  if (n === 0) {
    return {
      gains: [],
      distances: [],
      closestSpeaker: null,
      rawGains: [],
      metadata: { speakerCount: 0, source: { x: srcX, y: srcY } }
    };
  }

  if (n === 1) {
    const spk = speakers[0];
    const dist = calculateDistance(srcX, srcY, spk.x ?? 0, spk.y ?? 0);
    return {
      gains: [1.0],
      distances: [dist],
      closestSpeaker: spk,
      rawGains: [1.0],
      metadata: {
        speakerCount: 1,
        source: { x: srcX, y: srcY },
        description: `Single speaker active (${spk.sinkName || spk.sinkId})`
      }
    };
  }

  const distances = new Array(n);
  const rawGains = new Array(n);
  let minDistance = Infinity;
  let closestIdx = 0;

  for (let i = 0; i < n; i++) {
    const spk = speakers[i];
    const sx = Math.max(-1.0, Math.min(1.0, spk.x ?? 0.0));
    const sy = Math.max(-1.0, Math.min(1.0, spk.y ?? 0.0));

    const dist = calculateDistance(srcX, srcY, sx, sy);
    distances[i] = dist;

    if (dist < minDistance) {
      minDistance = dist;
      closestIdx = i;
    }

    rawGains[i] = calculateSpatialAttenuation(dist, falloff);
  }

  const finalGains = shouldNormalize ? normalizeSpatialGains(rawGains) : rawGains;
  const closestSpeaker = speakers[closestIdx] || null;

  return {
    gains: finalGains,
    distances,
    closestSpeaker,
    rawGains,
    metadata: {
      speakerCount: n,
      source: { x: srcX, y: srcY },
      closestSpeakerName: closestSpeaker?.sinkName || closestSpeaker?.sinkId || 'Unknown',
      minDistance,
      falloff,
      description: `Source at (${srcX >= 0 ? '+' : ''}${srcX.toFixed(2)}, ${srcY >= 0 ? '+' : ''}${srcY.toFixed(2)}) → Nearest: ${closestSpeaker?.sinkName || 'Speaker'}`
    }
  };
}

/**
 * Calculates continuous Orbit trajectory coordinates.
 *
 * @param {number} phase - Phase in [0, 1) representing cycle progress
 * @param {number} [radius=0.7] - Orbit radius in [0.1, 1.0]
 * @param {object} [center={x: 0, y: 0}] - Center coordinates
 * @param {'forward' | 'reverse'} [direction='forward']
 * @returns {{x: number, y: number}}
 */
function calculateOrbitPosition(phase, radius = 0.7, center = { x: 0, y: 0 }, direction = 'forward') {
  const r = Math.max(0.05, Math.min(1.0, typeof radius === 'number' && !isNaN(radius) ? radius : 0.7));
  const cx = Math.max(-1.0, Math.min(1.0, center?.x ?? 0.0));
  const cy = Math.max(-1.0, Math.min(1.0, center?.y ?? 0.0));

  const normPhase = ((phase % 1.0) + 1.0) % 1.0;
  // Forward: Clockwise (starts at top y=1, moves right x=1)
  // Angle theta: 0 -> top (0, r), 0.25 -> right (r, 0), 0.5 -> bottom (0, -r), 0.75 -> left (-r, 0)
  const angle = direction === 'reverse'
    ? Math.PI / 2 + normPhase * 2 * Math.PI
    : Math.PI / 2 - normPhase * 2 * Math.PI;

  const x = Math.max(-1.0, Math.min(1.0, cx + r * Math.cos(angle)));
  const y = Math.max(-1.0, Math.min(1.0, cy + r * Math.sin(angle)));

  return { x, y };
}

/**
 * Calculates continuous Horizontal Sweep coordinates.
 * Oscillates Left (-1) <-> Right (+1) smoothly at a configurable Y position.
 *
 * @param {number} phase - Phase in [0, 1) representing full round-trip cycle (Left -> Right -> Left)
 * @param {number} [fixedY=0.0] - Fixed Y position in [-1, +1]
 * @param {'forward' | 'reverse'} [direction='forward']
 * @returns {{x: number, y: number}}
 */
function calculateHorizontalSweepPosition(phase, fixedY = 0.0, direction = 'forward') {
  const y = Math.max(-1.0, Math.min(1.0, typeof fixedY === 'number' && !isNaN(fixedY) ? fixedY : 0.0));
  const normPhase = ((phase % 1.0) + 1.0) % 1.0;

  // Triangle wave between -1 and +1
  let x;
  if (normPhase < 0.5) {
    // 0.0 to 0.5: -1 to +1
    x = -1.0 + 4.0 * normPhase;
  } else {
    // 0.5 to 1.0: +1 to -1
    x = 1.0 - 4.0 * (normPhase - 0.5);
  }

  if (direction === 'reverse') {
    x = -x;
  }

  return { x: Math.max(-1.0, Math.min(1.0, x)), y };
}

/**
 * Calculates continuous Vertical Sweep coordinates.
 * Oscillates Back (-1) <-> Front (+1) smoothly at a configurable X position.
 *
 * @param {number} phase - Phase in [0, 1) representing full round-trip cycle (Back -> Front -> Back)
 * @param {number} [fixedX=0.0] - Fixed X position in [-1, +1]
 * @param {'forward' | 'reverse'} [direction='forward']
 * @returns {{x: number, y: number}}
 */
function calculateVerticalSweepPosition(phase, fixedX = 0.0, direction = 'forward') {
  const x = Math.max(-1.0, Math.min(1.0, typeof fixedX === 'number' && !isNaN(fixedX) ? fixedX : 0.0));
  const normPhase = ((phase % 1.0) + 1.0) % 1.0;

  // Triangle wave between -1 and +1
  let y;
  if (normPhase < 0.5) {
    // 0.0 to 0.5: -1 (Back) to +1 (Front)
    y = -1.0 + 4.0 * normPhase;
  } else {
    // 0.5 to 1.0: +1 (Front) to -1 (Back)
    y = 1.0 - 4.0 * (normPhase - 0.5);
  }

  if (direction === 'reverse') {
    y = -y;
  }

  return { x, y: Math.max(-1.0, Math.min(1.0, y)) };
}

/**
 * Generates deterministic default 2D speaker coordinates for N speakers.
 *
 * Standard acoustic room geometries:
 * - N=1: Center (0, 0)
 * - N=2: Stereo pair (-0.8, +0.6) and (+0.8, +0.6)
 * - N=3: LCR or Front+Surrounds (-0.8, +0.6), (+0.8, +0.6), (0.0, -0.8)
 * - N=4: Quad layout: Front-Left (-0.8, 0.8), Front-Right (0.8, 0.8), Rear-Right (0.8, -0.8), Rear-Left (-0.8, -0.8)
 * - N>4: Regular polygon inscribed in radius 0.85
 *
 * @param {string[]} sinkIds - Ordered list of sink IDs
 * @returns {Record<string, {x: number, y: number}>}
 */
function getDefaultSpeakerPositions(sinkIds = []) {
  const positions = {};
  const n = sinkIds.length;

  if (n === 0) return positions;

  if (n === 1) {
    positions[sinkIds[0]] = { x: 0.0, y: 0.0 };
    return positions;
  }

  if (n === 2) {
    positions[sinkIds[0]] = { x: -0.8, y: 0.5 };
    positions[sinkIds[1]] = { x: 0.8, y: 0.5 };
    return positions;
  }

  if (n === 3) {
    positions[sinkIds[0]] = { x: -0.8, y: 0.6 };
    positions[sinkIds[1]] = { x: 0.8, y: 0.6 };
    positions[sinkIds[2]] = { x: 0.0, y: -0.8 };
    return positions;
  }

  if (n === 4) {
    positions[sinkIds[0]] = { x: -0.8, y: 0.8 };  // Front-Left (A)
    positions[sinkIds[1]] = { x: 0.8, y: 0.8 };   // Front-Right (B)
    positions[sinkIds[2]] = { x: 0.8, y: -0.8 };  // Rear-Right (C)
    positions[sinkIds[3]] = { x: -0.8, y: -0.8 }; // Rear-Left (D)
    return positions;
  }

  // N >= 5: Regular polygon distributed clockwise starting at top front
  const r = 0.85;
  for (let i = 0; i < n; i++) {
    const angle = Math.PI / 2 - (i / n) * 2 * Math.PI;
    const x = Math.round(r * Math.cos(angle) * 100) / 100;
    const y = Math.round(r * Math.sin(angle) * 100) / 100;
    positions[sinkIds[i]] = { x, y };
  }

  return positions;
}

module.exports = {
  calculateDistance,
  calculateSpatialAttenuation,
  normalizeSpatialGains,
  calculateSpatialGains,
  calculateOrbitPosition,
  calculateHorizontalSweepPosition,
  calculateVerticalSweepPosition,
  getDefaultSpeakerPositions
};
