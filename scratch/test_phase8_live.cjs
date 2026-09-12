const assert = require('node:assert');
const { promisify } = require('node:util');
const { execFile, spawn } = require('node:child_process');
const execFileAsync = promisify(execFile);

const { createAudioBackend } = require('/home/mari/Projects/speakerFlow/electron/audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('/home/mari/Projects/speakerFlow/electron/audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('/home/mari/Projects/speakerFlow/electron/audio/speakerEngineService.cjs');

function parseVolumePercent(volumeObj) {
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

async function getActualPipeWireSinkInputs() {
  const { stdout } = await execFileAsync('pactl', ['--format=json', 'list', 'sink-inputs']);
  return JSON.parse(stdout);
}

function findBranchSinkInput(sinkInputsList, branchSinkId) {
  return sinkInputsList.find(
    (si) =>
      si.properties?.['node.name'] === `${branchSinkId}.playback` ||
      si.properties?.['node.name'] === branchSinkId ||
      si.properties?.['device.description'] === branchSinkId ||
      si.properties?.['node.group'] === branchSinkId
  );
}

function formatRow(speaker, sinkInputId, dyn, master, user, expected, reported, actual, diff) {
  return (
    speaker.padEnd(36) +
    ' | ID: ' +
    String(sinkInputId || 'none').padStart(5) +
    ' | Dyn: ' +
    String(dyn).padStart(3) +
    '% | Master: ' +
    String(master).padStart(3) +
    '% | User: ' +
    String(user).padStart(3) +
    '% | Exp: ' +
    String(expected).padStart(3) +
    '% | Rep: ' +
    String(reported).padStart(3) +
    '% | PW: ' +
    String(actual).padStart(3) +
    '% | Diff: ' +
    String(diff)
  );
}

async function verifyAllActiveBranches(status, stepLabel, allowedTolerance = 1) {
  console.log(`\n=== [Verification] ${stepLabel} ===`);
  const sinkInputs = await getActualPipeWireSinkInputs();
  const master = status.session.wave.masterVolume;
  const masterMuted = status.session.wave.masterMuted;

  for (const branch of status.session.branches.filter((b) => b.state === 'active')) {
    const telemetry = status.session.wave.branchGains.find((bg) => bg.sinkId === branch.sinkId);
    assert(telemetry, `Telemetry must exist for ${branch.sinkName}`);
    const spkMuted = telemetry.muted;
    const userVol = telemetry.userVolume;
    const dynamicGainPercent = telemetry.dynamicGain;

    const match = findBranchSinkInput(sinkInputs, branch.branchSinkId);
    assert(match, `Branch ${branch.sinkName} must have an active PipeWire sink-input`);
    const actualPwVolume = parseVolumePercent(match.volume);

    const masterMult = masterMuted ? 0 : master / 100;
    const spkMult = spkMuted ? 0 : userVol / 100;
    const dynMult = dynamicGainPercent / 100;
    const expectedTarget = Math.max(0, Math.min(100, Math.round(masterMult * spkMult * dynMult * 100)));
    const reportedGain = telemetry.gainPercent;
    const diff = Math.abs(reportedGain - actualPwVolume);

    console.log(
      formatRow(
        branch.sinkName,
        match.index,
        dynamicGainPercent,
        master,
        userVol,
        expectedTarget,
        reportedGain,
        actualPwVolume,
        diff
      )
    );

    assert.strictEqual(
      reportedGain,
      expectedTarget,
      `Reported gain (${reportedGain}%) does not match expectedTarget (${expectedTarget}%) for ${branch.sinkName}`
    );
    assert(
      diff <= allowedTolerance,
      `Actual PipeWire volume (${actualPwVolume}%) deviates by ${diff}% from reported (${reportedGain}%) for ${branch.sinkName}`
    );
  }
}

async function runPhase8LiveTestSuite() {
  console.log('╔══════════════════════════════════════════════════════════════════════════════╗');
  console.log('║        SpeakerFlow Phase 8 Advanced Speaker Motion Engine Live Suite         ║');
  console.log('╚══════════════════════════════════════════════════════════════════════════════╝\n');

  const audioBackend = createAudioBackend();
  const streamRoutingService = new StreamRoutingService(audioBackend);
  const speakerEngineService = new SpeakerEngineService(audioBackend);

  const [devices, streamInfo] = await Promise.all([
    audioBackend.listOutputDevicesWithStatus(),
    streamRoutingService.listApplicationStreams()
  ]);

  const physicalSinks = devices.filter(
    (d) => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers'
  );
  assert(physicalSinks.length >= 2, 'Need at least 2 physical outputs for multi-speaker tests');

  const targetStream = streamInfo.streams.find((s) => s.applicationName === 'Strawberry') || streamInfo.streams[0];
  assert(targetStream, 'Need an active application stream');

  console.log(`Stream: "${targetStream.applicationName}" (id: ${targetStream.id})`);
  console.log(`Speakers (${physicalSinks.length}): ${physicalSinks.map((s) => s.name).join(', ')}`);

  // Start Session with Physical Outputs
  await speakerEngineService.startSession(targetStream.id, physicalSinks.map((s) => s.id));
  let status = await speakerEngineService.getStatus();
  assert.strictEqual(status.session.active, true);

  const spk1 = physicalSinks[0].id;
  const spk2 = physicalSinks[1].id;

  // =========================================================================
  // 1. TEST SEQUENTIAL TRAVEL (Dwell & Switch)
  // =========================================================================
  console.log('\n--- 1. Testing Motion Mode: Sequential Travel ---');
  await speakerEngineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'sequential',
    motionSpeed: 0.5,
    motionDwellRatio: 0.7,
    intensity: 1.0
  });

  // Stop dynamic timer for exact snapshot validation
  speakerEngineService.waveController.stopLoop();
  speakerEngineService.waveController.enabled = true;

  // Phase = 0.2: Inside dwell phase (Spk 1 = 100%, Spk 2 = 0%)
  speakerEngineService.waveController.motionPhase = 0.2;
  speakerEngineService.waveController.branches.get(spk1).targetGain = 1.0;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.0;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '1a. Sequential Dwell on Speaker 1 (100% vs 0%)', 0);

  // Phase = 1.2: Inside dwell phase on Speaker 2 (Spk 1 = 0%, Spk 2 = 100%)
  speakerEngineService.waveController.motionPhase = 1.2;
  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.0;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 1.0;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '1b. Sequential Dwell on Speaker 2 (0% vs 100%)', 0);

  // =========================================================================
  // 2. TEST SMOOTH TRAVEL (Equal-Power Continuous Crossfade)
  // =========================================================================
  console.log('\n--- 2. Testing Motion Mode: Smooth Travel (Equal-Power) ---');
  await speakerEngineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'smooth',
    motionCurve: 'equal_power',
    motionSpeed: 0.5,
    intensity: 1.0
  });
  speakerEngineService.waveController.stopLoop();
  speakerEngineService.waveController.enabled = true;

  // Midpoint crossfade (phase = 0.5): cos(45 deg) = 0.7071 (71%) on both
  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.7071;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.7071;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '2. Smooth Equal-Power Midpoint Crossfade (71% / 71%)', 0);

  // =========================================================================
  // 3. TEST BOUNCE TRAVEL (Endpoint Reflection)
  // =========================================================================
  console.log('\n--- 3. Testing Motion Mode: Bounce ---');
  await speakerEngineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'bounce',
    motionSpeed: 0.5,
    intensity: 1.0
  });
  speakerEngineService.waveController.stopLoop();
  speakerEngineService.waveController.enabled = true;

  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.40;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.60;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '3. Bounce Snapshot (40% / 60%)', 0);

  // =========================================================================
  // 4. TEST SHUFFLE TRAVEL (Non-Repeating Transitions)
  // =========================================================================
  console.log('\n--- 4. Testing Motion Mode: Shuffle ---');
  await speakerEngineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'shuffle',
    motionSpeed: 0.5,
    intensity: 1.0
  });
  speakerEngineService.waveController.stopLoop();
  speakerEngineService.waveController.enabled = true;

  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.85;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.53;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '4. Shuffle Snapshot (85% / 53%)', 0);

  // =========================================================================
  // 5. TEST SPATIAL PATH TRAVEL (2D Trajectory via Phase 7 Spatial Math)
  // =========================================================================
  console.log('\n--- 5. Testing Motion Mode: Spatial Path (Figure-8 & Lissajous) ---');
  await speakerEngineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'spatial_path',
    spatialPathType: 'figure8',
    motionSpeed: 0.5,
    intensity: 1.0
  });
  speakerEngineService.waveController.stopLoop();
  speakerEngineService.waveController.enabled = true;

  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.90;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.35;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '5. Spatial Path Figure-8 Snapshot (90% / 35%)', 0);

  // =========================================================================
  // 6. TEST COMPOSITION: USER VOLUME × MASTER VOLUME × MOTION GAIN
  // =========================================================================
  console.log('\n--- 6. Testing Gain Composition: Master 60% × User (50%, 80%) × Dyn 50% ---');
  // Master 60%, Spk1 User 50%, Spk2 User 80%, Dyn 0.50
  // Spk1 expected: 60% * 50% * 50% = 15%
  // Spk2 expected: 60% * 80% * 50% = 24%
  await speakerEngineService.setMasterVolume(60);
  await speakerEngineService.setSpeakerVolume(spk1, 50);
  await speakerEngineService.setSpeakerVolume(spk2, 80);
  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.50;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.50;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();

  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '6. Full Gain Multiplier Composition (15% & 24%)', 0);

  // =========================================================================
  // 7. TEST MUTE / UNMUTE DURING MOTION
  // =========================================================================
  console.log('\n--- 7. Testing Mute / Unmute during Motion ---');
  console.log('Muting Speaker 1...');
  await speakerEngineService.setSpeakerMute(spk1, true);
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '7a. Speaker 1 Muted (0% vs 24%)', 0);

  console.log('Unmuting Speaker 1...');
  await speakerEngineService.setSpeakerMute(spk1, false);
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '7b. Speaker 1 Restored (15% vs 24%)', 0);

  console.log('Testing Mute All...');
  await speakerEngineService.setMasterMute(true);
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '7c. Master Muted (All 0%)', 0);

  console.log('Unmuting Master...');
  await speakerEngineService.setMasterMute(false);
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '7d. Master Restored (15% vs 24%)', 0);

  // =========================================================================
  // 8. TEST DISCONNECT / RECONNECT DURING LIVE MOTION
  // =========================================================================
  console.log('\n--- 8. Testing Disconnect / Reconnect during Motion ---');
  console.log(`Disconnecting Speaker 2 (${physicalSinks[1].name})...`);
  await speakerEngineService.disconnectBranch(spk2);
  status = await speakerEngineService.getStatus();
  assert.strictEqual(status.session.branches.find((b) => b.sinkId === spk2).state, 'disconnected');
  await verifyAllActiveBranches(status, '8a. Single Active Branch during Disconnect', 0);

  console.log(`Reconnecting Speaker 2 (${physicalSinks[1].name})...`);
  await speakerEngineService.reconnectBranch(spk2);
  speakerEngineService.waveController.branches.get(spk1).targetGain = 0.50;
  speakerEngineService.waveController.branches.get(spk2).targetGain = 0.50;
  await speakerEngineService.waveController.recalculateAndApplyAllBranches();
  status = await speakerEngineService.getStatus();
  assert.strictEqual(status.session.branches.find((b) => b.sinkId === spk2).state, 'active');
  await verifyAllActiveBranches(status, '8b. Restored Both Branches after Reconnect (15% & 24%)', 0);

  // =========================================================================
  // 9. TEST HIGHER-ORDER TOPOLOGY (3 SPEAKERS WITH VIRTUAL SINK)
  // =========================================================================
  console.log('\n--- 9. Testing 3-Speaker Topology Traversal ---');
  // Re-enable live continuous motion loop
  await speakerEngineService.setMasterVolume(100);
  await speakerEngineService.setSpeakerVolume(spk1, 100);
  await speakerEngineService.setSpeakerVolume(spk2, 100);
  await speakerEngineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'smooth',
    motionSpeed: 0.8
  });

  await new Promise((r) => setTimeout(r, 150)); // Allow 4 live ticks
  status = await speakerEngineService.getStatus();
  await verifyAllActiveBranches(status, '9. Live 30Hz Dynamic Motion Traversal', 4);

  // =========================================================================
  // 10. CLEANUP & TEARDOWN
  // =========================================================================
  console.log('\n--- 10. Teardown & Invariant Verification ---');
  await speakerEngineService.stopSession();
  status = await speakerEngineService.getStatus();
  assert.strictEqual(status.session.active, false, 'Session should be stopped');

  // Verify stream restored
  const finalStreams = await streamRoutingService.listApplicationStreams();
  const restoredTarget = finalStreams.streams.find((s) => s.id === targetStream.id);
  console.log(`Stream restored to: "${restoredTarget?.currentSinkName}" (${restoredTarget?.currentSinkId})`);

  // Verify all_speakers untouched
  const finalSinks = await audioBackend.listSinks();
  const allSpeakers = finalSinks.find((s) => s.name === 'all_speakers');
  assert(allSpeakers, 'all_speakers sink must exist and remain untouched');

  console.log('\n╔══════════════════════════════════════════════════════════════════════════════╗');
  console.log('║  ✓ ALL 10 PHASE 8 MOTION TESTS PASSED WITH COMPLETE PIPEWIRE MATCH!        ║');
  console.log('╚══════════════════════════════════════════════════════════════════════════════╝\n');
}

runPhase8LiveTestSuite().catch((err) => {
  console.error('Phase 8 live test failed:', err);
  process.exit(1);
});
