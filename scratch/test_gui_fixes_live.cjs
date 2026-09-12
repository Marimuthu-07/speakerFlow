const assert = require('node:assert');
const { promisify } = require('node:util');
const { execFile } = require('node:child_process');
const execFileAsync = promisify(execFile);

const { createAudioBackend } = require('../electron/audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('../electron/audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('../electron/audio/speakerEngineService.cjs');

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

async function runLiveTest() {
  console.log('=================================================================');
  console.log('   SPEAKERFLOW LIVE BACKEND & MASTER GAIN VERIFICATION SUITE     ');
  console.log('=================================================================\n');

  const backend = createAudioBackend();
  const streamService = new StreamRoutingService(backend);
  const engineService = new SpeakerEngineService(backend);

  // 1. Check Stream & Physical Outputs
  console.log('1. Checking Strawberry stream & Physical Outputs...');
  const [streamsRes, devicesRes] = await Promise.all([
    streamService.listApplicationStreams(),
    backend.listOutputDevicesWithStatus()
  ]);

  const strawberry = streamsRes.streams.find(s => s.applicationName === 'Strawberry') || streamsRes.streams[0];
  assert(strawberry, 'Strawberry or audio playback stream must be available');
  console.log(`✓ Stream found: "${strawberry.applicationName}" (ID: ${strawberry.id}, sink: ${strawberry.currentSinkId})`);

  const physicalOutputs = devicesRes.filter(
    d => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers'
  );
  assert(physicalOutputs.length >= 2, 'At least 2 physical outputs required');
  console.log(`✓ Physical speakers (${physicalOutputs.length}): ${physicalOutputs.map(d => d.name).join(' & ')}`);

  const spkIds = physicalOutputs.map(d => d.id);

  // 2. Start Engine Session
  console.log('\n2. Starting SpeakerFlow Multi-Speaker Engine Session...');
  await engineService.startSession(strawberry.id, spkIds);
  let status = await engineService.getStatus();
  assert.strictEqual(status.session.active, true, 'Engine session must be active');
  assert.strictEqual(status.session.activeBranchCount, 2, 'Must have 2 active branches');
  console.log('✓ Multi-Speaker Engine started successfully with ingress/tap/branches.');

  const branch0 = status.session.branches[0];
  const branch1 = status.session.branches[1];

  // Helper verification function
  async function verifyBranchVolumes(expectedTarget0, expectedTarget1, label) {
    const sinkInputs = await getActualPipeWireSinkInputs();
    const si0 = findBranchSinkInput(sinkInputs, branch0.branchSinkId);
    const si1 = findBranchSinkInput(sinkInputs, branch1.branchSinkId);
    assert(si0, `Sink input for branch 0 (${branch0.sinkName}) must exist`);
    assert(si1, `Sink input for branch 1 (${branch1.sinkName}) must exist`);

    const actVol0 = parseVolumePercent(si0.volume);
    const actVol1 = parseVolumePercent(si1.volume);

    console.log(`[${label}]`);
    console.log(`  Branch 0 (${branch0.sinkName}): Expected = ${expectedTarget0}%, Actual PipeWire = ${actVol0}%`);
    console.log(`  Branch 1 (${branch1.sinkName}): Expected = ${expectedTarget1}%, Actual PipeWire = ${actVol1}%`);

    assert(Math.abs(actVol0 - expectedTarget0) <= 1, `Branch 0 volume mismatch: expected ${expectedTarget0}%, got ${actVol0}%`);
    assert(Math.abs(actVol1 - expectedTarget1) <= 1, `Branch 1 volume mismatch: expected ${expectedTarget1}%, got ${actVol1}%`);
  }

  // 3. Test Master Volume 100% -> 50% -> 20%
  console.log('\n3. Testing Master Volume scaling (100% → 50% → 20%)...');
  await engineService.setMasterVolume(100);
  await verifyBranchVolumes(100, 100, 'Master 100% (Default)');

  await engineService.setMasterVolume(50);
  await verifyBranchVolumes(50, 50, 'Master 50%');

  await engineService.setMasterVolume(20);
  await verifyBranchVolumes(20, 20, 'Master 20%');

  // 4. Test Mute All -> all branches forced to 0%
  console.log('\n4. Testing Master Mute All...');
  await engineService.setMasterMute(true);
  await verifyBranchVolumes(0, 0, 'Master Mute (All branches forced to 0%)');

  // 5. Test Unmute -> restore previous effective gain (20%)
  console.log('\n5. Testing Master Unmute...');
  await engineService.setMasterMute(false);
  await verifyBranchVolumes(20, 20, 'Master Unmute (Restores 20%)');

  // 6. Test Master Volume authoritativeness during active Motion Engine ticks
  console.log('\n6. Testing Master Volume during active Motion Engine (Phase 8)...');
  await engineService.setMasterVolume(50); // Master = 50%
  await engineService.setMotionConfig({
    enabled: true,
    engineMode: 'motion',
    motionMode: 'smooth',
    motionCurve: 'equal_power',
    motionSpeed: 1.0,
    intensity: 1.0
  });

  // Let motion loop run for 1 second and sample PipeWire volumes
  console.log('Sampling live PipeWire branch volumes during continuous crossfade with Master = 50%...');
  for (let s = 1; s <= 4; s++) {
    await new Promise(r => setTimeout(r, 250));
    const sinkInputs = await getActualPipeWireSinkInputs();
    const si0 = findBranchSinkInput(sinkInputs, branch0.branchSinkId);
    const si1 = findBranchSinkInput(sinkInputs, branch1.branchSinkId);
    const v0 = parseVolumePercent(si0.volume);
    const v1 = parseVolumePercent(si1.volume);
    console.log(`  Sample ${s} (t = ${s * 250}ms): Branch 0 = ${v0}%, Branch 1 = ${v1}% (All ≤ Master 50%)`);
    assert(v0 <= 51, `Branch 0 volume (${v0}%) must not exceed Master (50%)`);
    assert(v1 <= 51, `Branch 1 volume (${v1}%) must not exceed Master (50%)`);
  }

  // Disable motion engine
  await engineService.setWaveConfig({ enabled: false });

  // 7. Stop Session and verify original routing restoration
  console.log('\n7. Stopping Engine Session and verifying restoration...');
  await engineService.stopSession();
  const finalStatus = await engineService.getStatus();
  assert.strictEqual(finalStatus.session.active, false, 'Session must be inactive');

  const restoredStream = await engineService.findApplicationStream(strawberry.id);
  assert(restoredStream, 'Stream must still exist');
  console.log(`✓ Stream restored to sink: "${restoredStream.currentSinkId}" (original: "${strawberry.currentSinkId}")`);
  assert.strictEqual(restoredStream.currentSinkId, strawberry.currentSinkId, 'Stream must be restored to original sink');

  console.log('\n=================================================================');
  console.log('   ALL BACKEND & MASTER GAIN LIVE VERIFICATIONS PASSED 100%!     ');
  console.log('=================================================================\n');
}

runLiveTest().catch((err) => {
  console.error('\n❌ LIVE TEST FAILED:', err);
  process.exit(1);
});
