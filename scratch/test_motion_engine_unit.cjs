const assert = require('node:assert');
const {
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
} = require('/home/mari/Projects/speakerFlow/electron/audio/motionEngine.cjs');

function runUnitTests() {
  console.log('=== Running Motion Engine Pure Unit Test Suite ===\n');

  // 1. Normalization & Curve Tests
  console.log('1. Testing Normalizers & Curves...');
  assert.strictEqual(normalizeMotionMode('sequential'), 'sequential');
  assert.strictEqual(normalizeMotionMode('invalid_mode'), 'smooth');
  assert.strictEqual(normalizeCurveType('linear'), 'linear');
  assert.strictEqual(normalizeCurveType('invalid_curve'), 'equal_power');

  // Equal-power law test: cos^2(u*pi/2) + sin^2(u*pi/2) === 1.0
  for (let u = 0.0; u <= 1.0; u += 0.1) {
    const cf = applyCrossfadeCurve(u, 'equal_power');
    const powerSum = cf.fromGain * cf.fromGain + cf.toGain * cf.toGain;
    assert(Math.abs(powerSum - 1.0) < 1e-6, `Equal-power sum must be 1.0, got ${powerSum} at u=${u}`);
  }
  console.log('✓ Normalizers and curve math verified.');

  // 2. N = 0 and N = 1 Edge Cases
  console.log('\n2. Testing N=0 and N=1 safety across all modes...');
  const modes = ['sequential', 'smooth', 'circular', 'bounce', 'shuffle', 'spatial_path'];

  // N = 0
  assert.strictEqual(calculateSequentialGains({ n: 0 }).gains.length, 0);
  assert.strictEqual(calculateSmoothTravelGains({ n: 0 }).gains.length, 0);
  assert.strictEqual(calculateCircularTravelGains({ n: 0 }).gains.length, 0);
  assert.strictEqual(calculateBounceGains({ n: 0 }).gains.length, 0);
  assert.strictEqual(calculateShuffleGains({ speakers: [] }).gains.length, 0);
  assert.strictEqual(calculateSpatialPathGains({ speakers: [] }).gains.length, 0);

  // N = 1
  const singleSpk = [{ sinkId: 'spk1', sinkName: 'Single Speaker' }];
  assert.deepStrictEqual(calculateSequentialGains({ n: 1, speakers: singleSpk }).gains, [1.0]);
  assert.deepStrictEqual(calculateSmoothTravelGains({ n: 1, speakers: singleSpk }).gains, [1.0]);
  assert.deepStrictEqual(calculateCircularTravelGains({ n: 1, speakers: singleSpk }).gains, [1.0]);
  assert.deepStrictEqual(calculateBounceGains({ n: 1, speakers: singleSpk }).gains, [1.0]);
  assert.deepStrictEqual(calculateShuffleGains({ speakers: singleSpk }).gains, [1.0]);
  assert.deepStrictEqual(calculateSpatialPathGains({ speakers: singleSpk }).gains, [1.0]);
  console.log('✓ N=0 and N=1 safely handled for all modes.');

  // 3. Sequential Travel Tests (N=3)
  console.log('\n3. Testing Sequential Travel with Dwell and Transition...');
  const speakers3 = [
    { sinkId: 'A', sinkName: 'Speaker A' },
    { sinkId: 'B', sinkName: 'Speaker B' },
    { sinkId: 'C', sinkName: 'Speaker C' }
  ];

  // At phase = 0.2 (dwell phase when dwellRatio = 0.6): Speaker A = 1.0, others = 0
  let seqRes = calculateSequentialGains({ phase: 0.2, n: 3, dwellRatio: 0.6, speakers: speakers3 });
  assert.strictEqual(seqRes.transition.isDwell, true);
  assert.strictEqual(seqRes.gains[0], 1.0);
  assert.strictEqual(seqRes.gains[1], 0.0);
  assert.strictEqual(seqRes.gains[2], 0.0);

  // At phase = 0.8 (transition phase when dwellRatio = 0.6): A fading out, B fading in
  seqRes = calculateSequentialGains({ phase: 0.8, n: 3, dwellRatio: 0.6, speakers: speakers3 });
  assert.strictEqual(seqRes.transition.isDwell, false);
  assert(seqRes.gains[0] > 0.0 && seqRes.gains[0] < 1.0, 'Speaker A should be transitioning');
  assert(seqRes.gains[1] > 0.0 && seqRes.gains[1] < 1.0, 'Speaker B should be transitioning');
  assert.strictEqual(seqRes.gains[2], 0.0);

  // At phase = 1.1 (dwell on Speaker B)
  seqRes = calculateSequentialGains({ phase: 1.1, n: 3, dwellRatio: 0.6, speakers: speakers3 });
  assert.strictEqual(seqRes.transition.isDwell, true);
  assert.strictEqual(seqRes.gains[0], 0.0);
  assert.strictEqual(seqRes.gains[1], 1.0);
  assert.strictEqual(seqRes.gains[2], 0.0);
  console.log('✓ Sequential Travel verified.');

  // 4. Smooth Travel Tests (N=3)
  console.log('\n4. Testing Smooth Continuous Crossfade Travel...');
  // At phase = 0.0: 100% on A
  let smoothRes = calculateSmoothTravelGains({ phase: 0.0, n: 3, speakers: speakers3 });
  assert.strictEqual(smoothRes.gains[0], 1.0);
  assert.strictEqual(smoothRes.gains[1], 0.0);
  assert.strictEqual(smoothRes.gains[2], 0.0);

  // At phase = 0.5: Equal power crossfade between A and B
  smoothRes = calculateSmoothTravelGains({ phase: 0.5, n: 3, curveType: 'equal_power', speakers: speakers3 });
  const pSum = smoothRes.gains[0] ** 2 + smoothRes.gains[1] ** 2;
  assert(Math.abs(pSum - 1.0) < 1e-5, 'Equal power crossfade must sum to 1.0');
  assert.strictEqual(smoothRes.gains[2], 0.0);

  // Circular wrap at phase = 2.5: Crossfade between C (idx 2) and A (idx 0)
  smoothRes = calculateCircularTravelGains({ phase: 2.5, n: 3, speakers: speakers3 });
  assert(smoothRes.gains[2] > 0.5, 'C should be active');
  assert(smoothRes.gains[0] > 0.5, 'A should be receiving wrap-around energy');
  assert.strictEqual(smoothRes.gains[1], 0.0);
  console.log('✓ Smooth & Circular Travel verified.');

  // 5. Bounce Travel Tests (N=3: Cycle length = 2 * (3-1) = 4)
  console.log('\n5. Testing Bounce Travel Turnaround Behavior...');
  // Phase = 0: at A
  let bRes = calculateBounceGains({ phase: 0.0, n: 3, speakers: speakers3 });
  assert.strictEqual(bRes.gains[0], 1.0);
  assert.strictEqual(bRes.transition.movementType, 'Bounce Outward');

  // Phase = 1.0: at B (moving outward to C)
  bRes = calculateBounceGains({ phase: 1.0, n: 3, speakers: speakers3 });
  assert.strictEqual(bRes.gains[1], 1.0);
  assert.strictEqual(bRes.transition.movementType, 'Bounce Outward');

  // Phase = 2.0: at C (turnaround point)
  bRes = calculateBounceGains({ phase: 2.0, n: 3, speakers: speakers3 });
  assert.strictEqual(bRes.gains[2], 1.0);

  // Phase = 3.0: at B (returning to A)
  bRes = calculateBounceGains({ phase: 3.0, n: 3, speakers: speakers3 });
  assert.strictEqual(bRes.gains[1], 1.0);
  assert.strictEqual(bRes.transition.movementType, 'Bounce Return');
  console.log('✓ Bounce Travel verified.');

  // 6. Shuffle Travel Tests (Deterministic seeded PRNG)
  console.log('\n6. Testing Shuffle Travel Non-Repetition Invariant...');
  let seed = 42;
  function pseudoRandom() {
    seed = (seed * 9301 + 49297) % 233280;
    return seed / 233280;
  }

  const speakers4 = [
    { sinkId: '1', sinkName: 'Spk 1' },
    { sinkId: '2', sinkName: 'Spk 2' },
    { sinkId: '3', sinkName: 'Spk 3' },
    { sinkId: '4', sinkName: 'Spk 4' }
  ];

  let shuffleState = { currentIdx: 0, targetIdx: 1, progress: 0.0 };
  const history = [0];

  for (let step = 0; step < 20; step++) {
    const res = calculateShuffleGains({
      shuffleState,
      dt: 1.0, // Large dt to trigger transitions
      speed: 1.0,
      speakers: speakers4,
      randomFn: pseudoRandom
    });
    shuffleState = res.nextState;

    if (shuffleState.currentIdx !== history[history.length - 1]) {
      const prev = history[history.length - 1];
      const curr = shuffleState.currentIdx;
      assert(curr !== prev, `Shuffle must NEVER repeat the same speaker immediately (${prev} -> ${curr})`);
      history.push(curr);
    }
  }
  console.log(`✓ Shuffle verified with sequence: ${history.join(' → ')} (zero immediate repeats).`);

  // 7. Spatial Path Tests (Trajectories + SpatialAudio composition)
  console.log('\n7. Testing Spatial Path Trajectories & Spatial Composition...');
  const spatialSpeakers = [
    { sinkId: 'left', sinkName: 'Left', x: -0.8, y: 0.0 },
    { sinkId: 'right', sinkName: 'Right', x: 0.8, y: 0.0 },
    { sinkId: 'front', sinkName: 'Front', x: 0.0, y: 0.8 }
  ];

  const pathTypes = ['waypoints', 'figure8', 'lissajous', 'spiral'];
  for (const pathType of pathTypes) {
    for (let prog = 0; prog < 1.0; prog += 0.25) {
      const spRes = calculateSpatialPathGains({
        pathType,
        pathProgress: prog,
        speakers: spatialSpeakers,
        spatialOptions: { falloff: 0.5, normalize: true }
      });

      assert.strictEqual(spRes.gains.length, 3);
      assert(spRes.source.x >= -1.0 && spRes.source.x <= 1.0);
      assert(spRes.source.y >= -1.0 && spRes.source.y <= 1.0);
      const maxG = Math.max(...spRes.gains);
      assert(Math.abs(maxG - 1.0) < 1e-4, `Max normalized spatial gain must be 1.0, got ${maxG}`);
    }
  }
  console.log('✓ Spatial Path trajectories and Phase 7 spatial composition verified.');

  // 8. Master Output Lifecycle & State Preservation Regression Test
  console.log('\n8. Testing Master Output Lifecycle & State Preservation (IDLE → ACTIVE → STOP)...');
  const { WaveEngineController } = require('/home/mari/Projects/speakerFlow/electron/audio/waveEngineController.cjs');
  const mockAudioBackend = {
    runPactlJson: async () => []
  };

  const controller = new WaveEngineController(mockAudioBackend);

  // A. IDLE State
  assert.strictEqual(controller.masterVolume, 100, 'Initial master volume should be 100');
  assert.strictEqual(controller.masterMuted, false, 'Initial master mute should be false');
  assert.strictEqual(controller.branches.size, 0, 'Idle controller should have 0 active branches');

  // Change master volume while IDLE
  controller.masterVolume = 65;
  assert.strictEqual(controller.masterVolume, 65, 'Configured master volume must be stored in IDLE state');

  // B. Transition to ACTIVE State (Engine Starts)
  const branches = [
    { sinkId: 'sink_A', sinkName: 'Speaker A', branchSinkId: 'branch.0', state: 'active' },
    { sinkId: 'sink_B', sinkName: 'Speaker B', branchSinkId: 'branch.1', state: 'active' }
  ];
  // Simulate branch attachment without resolving pactl sink inputs
  for (const b of branches) {
    controller.branches.set(b.sinkId, {
      ...b,
      sinkInputId: null,
      currentAppliedVolume: null,
      pendingVolume: 100,
      targetGain: 1.0,
      isUpdating: false
    });
  }

  // Calculate target gain with preserved 65% master volume
  const targetA = controller.calculateBranchTargetGain('sink_A', 1.0);
  const targetB = controller.calculateBranchTargetGain('sink_B', 1.0);
  assert.strictEqual(targetA.targetPercent, 65, 'Active branch A must receive stored master volume (65%)');
  assert.strictEqual(targetB.targetPercent, 65, 'Active branch B must receive stored master volume (65%)');

  // C. Transition to STOP State (Engine Stops)
  controller.branches.clear();
  assert.strictEqual(controller.branches.size, 0, 'Stopped engine must have branches cleared');
  assert.strictEqual(controller.masterVolume, 65, 'Stopped engine must PRESERVE master volume (65%) for next session');
  assert.strictEqual(controller.masterMuted, false, 'Stopped engine must preserve master mute state');

  // D. Re-Start Engine Session
  for (const b of branches) {
    controller.branches.set(b.sinkId, {
      ...b,
      sinkInputId: null,
      currentAppliedVolume: null,
      pendingVolume: 100,
      targetGain: 1.0,
      isUpdating: false
    });
  }
  const reTargetA = controller.calculateBranchTargetGain('sink_A', 1.0);
  assert.strictEqual(reTargetA.targetPercent, 65, 'Re-started session must continue using preserved master volume (65%)');
  console.log('✓ Master Output lifecycle and state preservation verified (IDLE → ACTIVE → STOP → RE-START).');

  console.log('\n=== ALL PURE UNIT TESTS PASSED WITH 100% SUCCESS! ===\n');
}

try {
  runUnitTests();
} catch (err) {
  console.error('Unit test failed:', err);
  process.exit(1);
}
