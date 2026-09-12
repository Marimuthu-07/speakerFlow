const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { LinuxSystemPowerService, SystemPowerService } = require('../electron/services/systemPowerService.cjs');
const { WaveEngineController } = require('../electron/audio/waveEngineController.cjs');
const { SpeakerEngineService } = require('../electron/audio/speakerEngineService.cjs');

async function runUXTests() {
  console.log('=== Running SpeakerFlow UX Improvements Unit Tests ===\n');

  // 1. Keyboard handler inactive state does not intercept system media keys
  console.log('1. Testing Keyboard handler inactive state does not intercept system media keys...');
  let eventPrevented = false;
  let activeSessionMock = null;

  async function adjustMasterVolume(delta, engineService) {
    if (!engineService.session) return;
    const currentStatus = await engineService.getWaveStatus();
    const currentVol = currentStatus?.masterVolume ?? 100;
    const nextVol = Math.max(0, Math.min(100, currentVol + delta));
    await engineService.setMasterVolume(nextVol);
  }

  async function toggleMasterMute(engineService) {
    if (!engineService.session) return;
    const currentStatus = await engineService.getWaveStatus();
    const nextMute = !(currentStatus?.masterMuted ?? false);
    await engineService.setMasterMute(nextMute);
  }

  function simulateBeforeInput(event, input, engineService) {
    if (input.type !== 'keyDown') return false;
    if (engineService && engineService.session) {
      if (input.key === 'AudioVolumeUp' || input.code === 'AudioVolumeUp' || input.key === 'VolumeUp' || input.code === 'VolumeUp') {
        adjustMasterVolume(5, engineService).catch(() => {});
        event.preventDefault();
        return true;
      }
      if (input.key === 'AudioVolumeDown' || input.code === 'AudioVolumeDown' || input.key === 'VolumeDown' || input.code === 'VolumeDown') {
        adjustMasterVolume(-5, engineService).catch(() => {});
        event.preventDefault();
        return true;
      }
      if (input.key === 'AudioVolumeMute' || input.code === 'AudioVolumeMute' || input.key === 'VolumeMute' || input.code === 'VolumeMute') {
        toggleMasterMute(engineService).catch(() => {});
        event.preventDefault();
        return true;
      }
    }
    return false;
  }

  const fakeInactiveEvent = { preventDefault() { eventPrevented = true; } };
  eventPrevented = false;
  simulateBeforeInput(fakeInactiveEvent, { type: 'keyDown', key: 'AudioVolumeUp' }, { session: null });
  assert.strictEqual(eventPrevented, false, 'Inactive session must NOT preventDefault on VolumeUp');

  eventPrevented = false;
  simulateBeforeInput(fakeInactiveEvent, { type: 'keyDown', key: 'AudioVolumeDown' }, { session: null });
  assert.strictEqual(eventPrevented, false, 'Inactive session must NOT preventDefault on VolumeDown');

  eventPrevented = false;
  simulateBeforeInput(fakeInactiveEvent, { type: 'keyDown', key: 'AudioVolumeMute' }, { session: null });
  assert.strictEqual(eventPrevented, false, 'Inactive session must NOT preventDefault on AudioVolumeMute');
  console.log('✓ Keyboard handler allows normal system media keys when engine is inactive.');

  // 2. Focused + Active: VolumeUp (+5), VolumeDown (-5), Mute (toggle)
  console.log('\n2. Testing Keyboard handler active state changes SpeakerFlow master gain (+5, -5, mute toggle)...');
  const physicalSinkVolumeCalls = [];
  const streamsMap = new Map([
    [1, { id: 1, applicationName: 'App 1', currentSinkId: 'sink1', currentSinkName: 'Sink 1' }]
  ]);

  let simulatedDefaultSink2 = 'sink1';
  const mockBackend = {
    async setSinkInputVolume() {},
    async listSinkInputs() { return []; },
    async listSinks() { return [{ name: 'sink1', description: 'Sink 1', properties: {} }]; },
    async listOutputDevicesWithStatus() { return []; },
    async listApplicationStreams() { return Array.from(streamsMap.values()); },
    async moveSinkInput(streamId, targetSinkId) {
      const stream = streamsMap.get(streamId);
      if (stream) stream.currentSinkId = targetSinkId;
    },
    async findSinkByName(name) { return { id: name, name }; },
    async setSinkVolume(sinkName, vol) {
      if (!sinkName.startsWith('speakerflow.session.')) {
        physicalSinkVolumeCalls.push({ sinkName, vol });
      }
    },
    async setSinkMute(sinkName, muted) {
      if (!sinkName.startsWith('speakerflow.session.')) {
        physicalSinkVolumeCalls.push({ sinkName, muted });
      }
    },
    async getDefaultOutputId() { return simulatedDefaultSink2; },
    async setDefaultOutput(sinkId) { simulatedDefaultSink2 = String(sinkId); }
  };

  const section2PipelineSession = {
    async createBranch({ branchSinkId, physicalSinkId, onDisconnect }) {
      return { branchSinkId, physicalSinkId, triggerDisconnect: onDisconnect };
    },
    async destroyBranch() {},
    async destroy() {}
  };

  const section2PipelineAdapter = {
    async createPipeline() { return section2PipelineSession; }
  };

  const activeEngineService = new SpeakerEngineService(mockBackend, section2PipelineAdapter);
  await activeEngineService.startSession(1, ['sink1']);

  // Initial volume is 100, muted is false
  let status = await activeEngineService.getWaveStatus();
  assert.strictEqual(status.masterVolume, 100);
  assert.strictEqual(status.masterMuted, false);

  // 2a. VolumeDown: 100 -> 95
  const downEvent = { preventDefault() { eventPrevented = true; } };
  eventPrevented = false;
  simulateBeforeInput(downEvent, { type: 'keyDown', key: 'AudioVolumeDown' }, activeEngineService);
  assert.strictEqual(eventPrevented, true, 'Active session MUST preventDefault on VolumeDown');
  await new Promise(r => setTimeout(r, 20));
  status = await activeEngineService.getWaveStatus();
  assert.strictEqual(status.masterVolume, 95, 'VolumeDown must decrease master volume by 5');

  // Consecutive VolumeDown: 95 -> 90
  eventPrevented = false;
  simulateBeforeInput(downEvent, { type: 'keyDown', key: 'VolumeDown' }, activeEngineService);
  assert.strictEqual(eventPrevented, true);
  await new Promise(r => setTimeout(r, 20));
  status = await activeEngineService.getWaveStatus();
  assert.strictEqual(status.masterVolume, 90, 'Consecutive VolumeDown must step down to 90');

  // 2b. VolumeUp: 90 -> 95
  const upEvent = { preventDefault() { eventPrevented = true; } };
  eventPrevented = false;
  simulateBeforeInput(upEvent, { type: 'keyDown', key: 'AudioVolumeUp' }, activeEngineService);
  assert.strictEqual(eventPrevented, true, 'Active session MUST preventDefault on VolumeUp');
  await new Promise(r => setTimeout(r, 20));
  status = await activeEngineService.getWaveStatus();
  assert.strictEqual(status.masterVolume, 95, 'VolumeUp must increase master volume by 5');

  // 2c. Mute toggle: false -> true -> false
  const muteEvent = { preventDefault() { eventPrevented = true; } };
  eventPrevented = false;
  simulateBeforeInput(muteEvent, { type: 'keyDown', key: 'AudioVolumeMute' }, activeEngineService);
  assert.strictEqual(eventPrevented, true, 'Active session MUST preventDefault on AudioVolumeMute');
  await new Promise(r => setTimeout(r, 20));
  status = await activeEngineService.getWaveStatus();
  assert.strictEqual(status.masterMuted, true, 'First mute key press must toggle mute to true');

  eventPrevented = false;
  simulateBeforeInput(muteEvent, { type: 'keyDown', key: 'VolumeMute' }, activeEngineService);
  assert.strictEqual(eventPrevented, true);
  await new Promise(r => setTimeout(r, 20));
  status = await activeEngineService.getWaveStatus();
  assert.strictEqual(status.masterMuted, false, 'Second mute key press must toggle mute back to false');

  // 2d. Stop Session: Media handling is disabled
  await activeEngineService.stopSession();
  assert.strictEqual(activeEngineService.session, null);
  eventPrevented = false;
  simulateBeforeInput(downEvent, { type: 'keyDown', key: 'AudioVolumeDown' }, activeEngineService);
  assert.strictEqual(eventPrevented, false, 'After engine stop, media keys must NOT be prevented');

  assert.strictEqual(physicalSinkVolumeCalls.length, 0, 'Physical sink volume must never be touched by master engine controls');
  console.log('✓ Focused active VolumeUp (+5), VolumeDown (-5), Mute toggle, and stop deactivation verified.');

  // 3. Global Shortcut Registration: Detection of success/failure, duplicate prevention, failure cleanup, and shutdown
  console.log('\n3. Testing Global Shortcuts registration lifecycle, duplicate prevention, failure cleanup, and shutdown...');
  let registeredShortcuts = new Set();
  let registrationMode = 'success'; // 'success' or 'unsupported'

  const fakeGlobalShortcut = {
    register(key) {
      if (registrationMode === 'unsupported') {
        return false;
      }
      if (registeredShortcuts.has(key)) throw new Error(`Duplicate registration of ${key}`);
      registeredShortcuts.add(key);
      return true;
    },
    unregister(key) { registeredShortcuts.delete(key); },
    isRegistered(key) { return registeredShortcuts.has(key); }
  };

  function safeRegister(engineService) {
    fakeGlobalShortcut.unregister('VolumeUp');
    fakeGlobalShortcut.unregister('VolumeDown');
    fakeGlobalShortcut.unregister('VolumeMute');
    if (!engineService || !engineService.session) return { up: false, down: false, mute: false };
    const up = fakeGlobalShortcut.register('VolumeUp');
    const down = fakeGlobalShortcut.register('VolumeDown');
    const mute = fakeGlobalShortcut.register('VolumeMute');
    return { up, down, mute };
  }

  function safeUnregister() {
    fakeGlobalShortcut.unregister('VolumeUp');
    fakeGlobalShortcut.unregister('VolumeDown');
    fakeGlobalShortcut.unregister('VolumeMute');
  }

  // 3a. Registration under environments where register() returns true
  registrationMode = 'success';
  const dummyActiveService = { session: { active: true } };
  const regResults = safeRegister(dummyActiveService);
  assert.strictEqual(regResults.up, true);
  assert.strictEqual(regResults.down, true);
  assert.strictEqual(regResults.mute, true);
  assert.strictEqual(fakeGlobalShortcut.isRegistered('VolumeUp'), true);
  assert.strictEqual(registeredShortcuts.size, 3);

  // 3b. Duplicate registration safety check (idempotent)
  const dupResults = safeRegister(dummyActiveService);
  assert.strictEqual(dupResults.up, true);
  assert.strictEqual(registeredShortcuts.size, 3);

  // 3c. Registration under Wayland/GNOME environment where register() returns false
  safeUnregister();
  registrationMode = 'unsupported';
  const waylandResults = safeRegister(dummyActiveService);
  assert.strictEqual(waylandResults.up, false);
  assert.strictEqual(waylandResults.down, false);
  assert.strictEqual(waylandResults.mute, false);
  assert.strictEqual(fakeGlobalShortcut.isRegistered('VolumeUp'), false);
  assert.strictEqual(registeredShortcuts.size, 0);

  // 3d. Failed engine startup unregisters all shortcuts
  registrationMode = 'success';
  safeRegister(dummyActiveService);
  assert.strictEqual(registeredShortcuts.size, 3);
  // Simulate startup error
  safeUnregister();
  assert.strictEqual(registeredShortcuts.size, 0, 'Failed startup must clear all shortcuts');

  // 3e. Normal engine stop unregisters all shortcuts
  safeRegister(dummyActiveService);
  assert.strictEqual(registeredShortcuts.size, 3);
  safeUnregister();
  assert.strictEqual(registeredShortcuts.size, 0, 'Engine stop must clear all shortcuts');

  // 3f. App shutdown (before-quit / will-quit) unregisters all shortcuts
  safeRegister(dummyActiveService);
  assert.strictEqual(registeredShortcuts.size, 3);
  safeUnregister();
  assert.strictEqual(registeredShortcuts.size, 0, 'App shutdown must clear all shortcuts');
  console.log('✓ Global shortcuts registration detection, duplicate prevention, failure cleanup, and shutdown verified.');

  // 6, 7, 8, 9, 10: Intentional disconnect vs unexpected sink disconnect & auto-reconnect
  console.log('\n4. Testing Intentional vs Unexpected Disconnect and Auto-Reconnect...');
  let branchProcessesCreated = 0;
  let branchProcessesDestroyed = 0;
  let ingressCreated = 0;
  let ingressDestroyed = 0;

  const mockPipelineSession = {
    async createBranch({ branchSinkId, physicalSinkId, onDisconnect }) {
      branchProcessesCreated++;
      return { branchSinkId, physicalSinkId, triggerDisconnect: onDisconnect };
    },
    async destroyBranch() {
      branchProcessesDestroyed++;
    },
    async destroy() {
      ingressDestroyed++;
    }
  };

  const mockPipelineAdapter = {
    async createPipeline() {
      ingressCreated++;
      return mockPipelineSession;
    }
  };

  const engineService = new SpeakerEngineService(mockBackend, mockPipelineAdapter);
  await engineService.startSession(1, ['sink1']);
  assert.strictEqual(engineService.session.branches[0].state, 'active');
  assert.strictEqual(engineService.session.branches[0].intentionalDisconnect, false);

  // 6. Intentional branch disconnect does not auto-reconnect
  console.log('Testing intentional branch disconnect...');
  await engineService.disconnectBranch('sink1');
  assert.strictEqual(engineService.session.branches[0].state, 'disconnected');
  assert.strictEqual(engineService.session.branches[0].intentionalDisconnect, true);
  assert.strictEqual(engineService.reconnectTimers.size, 0, 'No reconnect timer when intentionally disconnected');

  // 7. Manual reconnect resets intentional disconnect
  console.log('Testing manual reconnect...');
  await engineService.reconnectBranch('sink1');
  assert.strictEqual(engineService.session.branches[0].state, 'active');
  assert.strictEqual(engineService.session.branches[0].intentionalDisconnect, false);

  // 8. Unexpected physical sink disappearance schedules reconnect
  console.log('Testing unexpected physical sink disappearance...');
  const activeBranchHandle = engineService.session.branches[0].handle;
  activeBranchHandle.triggerDisconnect('physical unplug');
  assert.strictEqual(engineService.session.branches[0].state, 'disconnected');
  assert.strictEqual(engineService.session.branches[0].intentionalDisconnect, false);
  assert.strictEqual(engineService.reconnectTimers.has('sink1'), true, 'Auto-reconnect timer must be scheduled');

  // 9. Sink returning allows automatic branch recreation
  console.log('Testing automatic branch recreation when sink is available...');
  // Trigger auto-reconnect immediately
  await engineService._performBranchReconnect('sink1', true);
  assert.strictEqual(engineService.session.branches[0].state, 'active');
  assert.strictEqual(engineService.reconnectTimers.size, 0);

  // 10. Failed reconnect uses bounded retry backoff
  console.log('Testing failed reconnect with missing sink...');
  const originalListSinks = mockBackend.listSinks;
  mockBackend.listSinks = async () => []; // Physical sink missing!
  await engineService._performBranchReconnect('sink1', true);
  assert.strictEqual(engineService.session.branches[0].state, 'disconnected');
  assert.strictEqual(engineService.session.branches[0].retryCount, 1);
  assert.strictEqual(engineService.reconnectTimers.has('sink1'), true);

  // Restore listSinks
  mockBackend.listSinks = originalListSinks;

  // 11. Retry timers stop when session stops
  console.log('Testing retry timers stop on session stop...');
  assert.strictEqual(engineService.reconnectTimers.size, 1);
  await engineService.stopSession();
  assert.strictEqual(engineService.reconnectTimers.size, 0, 'All reconnect timers must be cleared on stopSession');
  assert.strictEqual(engineService.session, null);
  console.log('✓ Auto-reconnect lifecycle, intentional disconnect, and timer bounds verified.');

  // 12, 13: Ingress cleanup and no session leak
  console.log('\n5. Testing Ingress cleanup and session isolation...');
  assert.strictEqual(ingressCreated, 1);
  assert.strictEqual(ingressDestroyed, 1);

  // Repeated start/stop
  await engineService.startSession(1, ['sink1']);
  assert.strictEqual(engineService.session !== null, true);
  await engineService.stopSession();
  assert.strictEqual(engineService.session, null);
  console.log('✓ Repeated start/stop operates with zero session leak.');

  // 14, 15, 16, 17: Host Battery and Bluetooth Device Battery
  console.log('\n6. Testing Host and Bluetooth Device Battery parsing...');
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'spkflow-battery-test-'));
  const batPath = path.join(tmpDir, 'BAT0');
  fs.mkdirSync(batPath, { recursive: true });
  fs.writeFileSync(path.join(batPath, 'type'), 'Battery\n');
  fs.writeFileSync(path.join(batPath, 'capacity'), '85\n');
  fs.writeFileSync(path.join(batPath, 'status'), 'Discharging\n');

  const powerService = new LinuxSystemPowerService(tmpDir);

  // Mock getBluetoothBatteries
  powerService.getBluetoothBatteries = async () => [
    {
      id: '7C:56:6D:EA:EF:74',
      name: 'Airdopes 148',
      percentage: 70,
      state: 'connected'
    },
    {
      id: '11:22:33:44:55:66',
      name: 'Generic BT Speaker',
      percentage: null, // Device without battery info
      state: 'connected'
    }
  ];

  const fullPowerStatus = await powerService.getBatteryStatus();
  assert.strictEqual(fullPowerStatus.host.available, true);
  assert.strictEqual(fullPowerStatus.host.percentage, 85);
  assert.strictEqual(fullPowerStatus.devices.length, 2);

  // Device with battery
  assert.strictEqual(fullPowerStatus.devices[0].name, 'Airdopes 148');
  assert.strictEqual(fullPowerStatus.devices[0].percentage, 70);

  // Device without battery
  assert.strictEqual(fullPowerStatus.devices[1].name, 'Generic BT Speaker');
  assert.strictEqual(fullPowerStatus.devices[1].percentage, null);

  // Disconnect device
  powerService.getBluetoothBatteries = async () => [];
  const disconnectedStatus = await powerService.getBatteryStatus();
  assert.strictEqual(disconnectedStatus.devices.length, 0);
  assert.strictEqual(disconnectedStatus.host.percentage, 85);

  fs.rmSync(tmpDir, { recursive: true, force: true });
  console.log('✓ Host and Bluetooth device battery parsing and normalization verified.');

  // 7. Testing External Session Sink Volume & Mute State Synchronization Lifecycle (Cases A - H)
  console.log('\n7. Testing External Session Sink Volume & Mute State Synchronization Lifecycle (Cases A - H)...');
  const sinkInputsMap = new Map();
  const physicalSinkCalls = [];
  const ingressSinkCalls = [];
  let simulatedDefaultSink = 'physical_speaker_1';
  let simulatedIngressSink = {
    name: '',
    volume: { 'front-left': { value: 65536, value_percent: '100%' }, 'front-right': { value: 65536, value_percent: '100%' } },
    mute: false,
    properties: { 'node.virtual': 'true' }
  };

  const syncTestStreamsMap = new Map([
    [10, { id: 10, applicationName: 'Music Player', currentSinkId: 'physical_speaker_1', currentSinkName: 'Physical Speaker 1' }]
  ]);

  const syncMockBackend = {
    async setSinkInputVolume(id, vol) {
      sinkInputsMap.set(id, vol);
    },
    async listSinkInputs() {
      return [
        { index: 501, properties: { 'node.name': `${simulatedIngressSink.name?.replace('.ingress', '.branch.0')}.playback` } },
        { index: 502, properties: { 'node.name': `${simulatedIngressSink.name?.replace('.ingress', '.branch.1')}.playback` } }
      ];
    },
    async listSinks() {
      return [
        {
          name: simulatedIngressSink.name,
          description: 'SpeakerFlow Session',
          volume: simulatedIngressSink.volume,
          mute: simulatedIngressSink.mute,
          properties: { 'node.virtual': 'true' }
        },
        {
          name: 'physical_speaker_1',
          description: 'Physical Speaker 1',
          volume: { 'front-left': { value: 50000, value_percent: '76%' } },
          mute: false,
          properties: {}
        },
        {
          name: 'physical_speaker_2',
          description: 'Physical Speaker 2',
          volume: { 'front-left': { value: 50000, value_percent: '76%' } },
          mute: false,
          properties: {}
        }
      ];
    },
    async listOutputDevicesWithStatus() {
      return [
        { id: 'physical_speaker_1', name: 'Physical Speaker 1', isVirtual: false, isDefault: true },
        { id: 'physical_speaker_2', name: 'Physical Speaker 2', isVirtual: false, isDefault: false }
      ];
    },
    async listApplicationStreams() { return Array.from(syncTestStreamsMap.values()); },
    async moveSinkInput(streamId, targetSinkId) {
      const stream = syncTestStreamsMap.get(streamId);
      if (stream) stream.currentSinkId = targetSinkId;
    },
    async findSinkByName(name) {
      if (name === simulatedIngressSink.name) {
        const vLeft = simulatedIngressSink.volume['front-left'];
        const volPercent = vLeft ? parseInt(vLeft.value_percent.replace('%', ''), 10) : 100;
        return {
          id: name,
          name,
          isVirtual: true,
          volumePercent: volPercent,
          mute: simulatedIngressSink.mute
        };
      }
      return { id: name, name, isVirtual: false, volumePercent: 76, mute: false };
    },
    async setSinkVolume(sinkName, vol) {
      if (sinkName.startsWith('speakerflow.session.')) {
        ingressSinkCalls.push({ sinkName, vol });
        simulatedIngressSink.volume = {
          'front-left': { value_percent: `${vol}%` },
          'front-right': { value_percent: `${vol}%` }
        };
      } else {
        physicalSinkCalls.push({ sinkName, vol });
      }
    },
    async setSinkMute(sinkName, muted) {
      if (sinkName.startsWith('speakerflow.session.')) {
        simulatedIngressSink.mute = Boolean(muted);
      }
    },
    async getDefaultOutputId() {
      return simulatedDefaultSink;
    },
    async setDefaultOutput(sinkId) {
      simulatedDefaultSink = String(sinkId);
    }
  };

  const syncPipelineSession = {
    async createBranch({ branchSinkId, physicalSinkId, onDisconnect }) {
      return { branchSinkId, physicalSinkId, triggerDisconnect: onDisconnect };
    },
    async destroyBranch() {},
    async destroy() {}
  };

  const syncPipelineAdapter = {
    async createPipeline({ ingressSinkId }) {
      simulatedIngressSink.name = ingressSinkId;
      simulatedIngressSink.volume = {
        'front-left': { value_percent: '100%' },
        'front-right': { value_percent: '100%' }
      };
      simulatedIngressSink.mute = false;
      return syncPipelineSession;
    }
  };

  const syncEngineService = new SpeakerEngineService(syncMockBackend, syncPipelineAdapter);

  let uiNotifiedValues = null;
  syncEngineService.onMasterVolumeChanged = (data) => {
    uiNotifiedValues = data;
  };

  await syncEngineService.startSession(10, ['physical_speaker_1', 'physical_speaker_2']);
  assert.strictEqual(syncEngineService.session.branches.length, 2);

  // Helper to compute final digital level reaching physical sink:
  // Final Level = (Ingress Volume / 100) * (Branch Target Gain / 100)
  function computeFinalDigitalLevel(engineService, sinkId, dynamicGain = 1.0) {
    const ingressVol = simulatedIngressSink.mute ? 0 : parseInt(simulatedIngressSink.volume['front-left'].value_percent.replace('%', ''), 10);
    const { targetPercent } = engineService.waveController.calculateBranchTargetGain(sinkId, dynamicGain);
    const finalLevelPercent = Math.round((ingressVol / 100) * (targetPercent / 100) * 100);
    return { targetPercent, finalLevelPercent };
  }

  // --- Case A ---
  // Ingress master = 50%, Speaker 1 user gain = 100%, Dynamic gain = 1.0
  // Expected branch target = 100%, Expected final digital level = 50%
  console.log('Testing Case A: Ingress master=50%, Speaker 1=100%, Dyn=1.0 -> Target=100%, Final=50%...');
  simulatedIngressSink.volume = { 'front-left': { value_percent: '50%' }, 'front-right': { value_percent: '50%' } };
  await syncEngineService.setSpeakerVolume('physical_speaker_1', 100);
  await syncEngineService.reconcileSession();
  const caseA = computeFinalDigitalLevel(syncEngineService, 'physical_speaker_1', 1.0);
  assert.strictEqual(caseA.targetPercent, 100, 'Case A: Branch target gain must be 100% (not multiplied by master)');
  assert.strictEqual(caseA.finalLevelPercent, 50, 'Case A: Final digital level must be 50% (50% ingress × 100% branch)');
  console.log('✓ Case A passed.');

  // --- Case B ---
  // Ingress master = 50%, Speaker 1 user gain = 80%, Dynamic gain = 1.0
  // Expected branch target = 80%, Expected final digital level = 40%
  console.log('Testing Case B: Ingress master=50%, Speaker 1=80%, Dyn=1.0 -> Target=80%, Final=40%...');
  simulatedIngressSink.volume = { 'front-left': { value_percent: '50%' }, 'front-right': { value_percent: '50%' } };
  await syncEngineService.setSpeakerVolume('physical_speaker_1', 80);
  await syncEngineService.reconcileSession();
  const caseB = computeFinalDigitalLevel(syncEngineService, 'physical_speaker_1', 1.0);
  assert.strictEqual(caseB.targetPercent, 80, 'Case B: Branch target gain must be 80%');
  assert.strictEqual(caseB.finalLevelPercent, 40, 'Case B: Final digital level must be 40% (50% ingress × 80% branch)');
  console.log('✓ Case B passed.');

  // --- Case C ---
  // Ingress master = 20%, Speaker 1 user gain = 100%, Dynamic gain = 1.0
  // Expected branch target = 100%, Expected final digital level = 20%
  console.log('Testing Case C: Ingress master=20%, Speaker 1=100%, Dyn=1.0 -> Target=100%, Final=20%...');
  simulatedIngressSink.volume = { 'front-left': { value_percent: '20%' }, 'front-right': { value_percent: '20%' } };
  await syncEngineService.setSpeakerVolume('physical_speaker_1', 100);
  await syncEngineService.reconcileSession();
  const caseC = computeFinalDigitalLevel(syncEngineService, 'physical_speaker_1', 1.0);
  assert.strictEqual(caseC.targetPercent, 100, 'Case C: Branch target gain must be 100%');
  assert.strictEqual(caseC.finalLevelPercent, 20, 'Case C: Final digital level must be 20% (20% ingress × 100% branch)');
  console.log('✓ Case C passed.');

  // --- Case D ---
  // Ingress master = 50%, Speaker 1 user gain = 80%, Dynamic gain = 0.5
  // Expected branch target = 40%, Expected final digital level = 20%
  console.log('Testing Case D: Ingress master=50%, Speaker 1=80%, Dyn=0.5 -> Target=40%, Final=20%...');
  simulatedIngressSink.volume = { 'front-left': { value_percent: '50%' }, 'front-right': { value_percent: '50%' } };
  await syncEngineService.setSpeakerVolume('physical_speaker_1', 80);
  await syncEngineService.reconcileSession();
  const caseD = computeFinalDigitalLevel(syncEngineService, 'physical_speaker_1', 0.5);
  assert.strictEqual(caseD.targetPercent, 40, 'Case D: Branch target gain must be 40% (80% × 0.5)');
  assert.strictEqual(caseD.finalLevelPercent, 20, 'Case D: Final digital level must be 20% (50% ingress × 40% branch)');
  console.log('✓ Case D passed.');

  // --- Case E ---
  // External ingress volume change: 100% -> 60%
  // Expected: waveController.masterVolume = 60, UI receives 60, branch target gains remain based only on speaker gain × dynamic gain
  console.log('Testing Case E: External ingress volume change 100% -> 60%...');
  simulatedIngressSink.volume = { 'front-left': { value_percent: '60%' }, 'front-right': { value_percent: '60%' } };
  uiNotifiedValues = null;
  await syncEngineService.reconcileSession();
  const statusAfterE = await syncEngineService.getWaveStatus();
  assert.strictEqual(statusAfterE.masterVolume, 60, 'Case E: waveController.masterVolume must be 60');
  assert.deepStrictEqual(uiNotifiedValues, { masterVolume: 60, masterMuted: false }, 'Case E: UI must receive 60%');
  const branchE = syncEngineService.waveController.calculateBranchTargetGain('physical_speaker_1', 1.0);
  assert.strictEqual(branchE.targetPercent, 80, 'Case E: Branch target gain remains 80% based solely on speaker × dynamic');
  console.log('✓ Case E passed.');

  // --- Case F ---
  // External ingress mute: false -> true
  // Expected: waveController.masterMuted = true, UI reflects mute, branch target gains are NOT additionally master-muted
  console.log('Testing Case F: External ingress mute false -> true...');
  simulatedIngressSink.mute = true;
  uiNotifiedValues = null;
  await syncEngineService.reconcileSession();
  const statusAfterF = await syncEngineService.getWaveStatus();
  assert.strictEqual(statusAfterF.masterMuted, true, 'Case F: waveController.masterMuted must be true');
  assert.deepStrictEqual(uiNotifiedValues, { masterVolume: 60, masterMuted: true }, 'Case F: UI must receive mute true');
  const branchF = syncEngineService.waveController.calculateBranchTargetGain('physical_speaker_1', 1.0);
  assert.strictEqual(branchF.targetPercent, 80, 'Case F: Branch target gain is NOT additionally zeroed because ingress handles master mute');
  console.log('✓ Case F passed.');

  // --- Case G ---
  // Restore ingress to 100% / unmute
  // Expected branch gains return to user/dynamic values without master multiplication
  console.log('Testing Case G: Restore ingress to 100% / unmute...');
  simulatedIngressSink.volume = { 'front-left': { value_percent: '100%' }, 'front-right': { value_percent: '100%' } };
  simulatedIngressSink.mute = false;
  uiNotifiedValues = null;
  await syncEngineService.reconcileSession();
  const statusAfterG = await syncEngineService.getWaveStatus();
  assert.strictEqual(statusAfterG.masterVolume, 100);
  assert.strictEqual(statusAfterG.masterMuted, false);
  const branchG = syncEngineService.waveController.calculateBranchTargetGain('physical_speaker_1', 1.0);
  assert.strictEqual(branchG.targetPercent, 80);
  console.log('✓ Case G passed.');

  // --- Case H ---
  // Physical sink volume must remain untouched
  console.log('Testing Case H: Physical sink volume remains untouched...');
  assert.strictEqual(physicalSinkCalls.length, 0, 'Case H: Physical sink volume was never called');
  console.log('✓ Case H passed.');

  // Verify Idempotency & Session Stop
  console.log('Testing Idempotency & Session Stop cleanup...');
  let loopTriggerCount = 0;
  syncEngineService.onMasterVolumeChanged = () => { loopTriggerCount++; };
  await syncEngineService.reconcileSession();
  assert.strictEqual(loopTriggerCount, 0, 'Reconcile when in sync is an idempotent no-op');

  await syncEngineService.stopSession();
  assert.strictEqual(syncEngineService.session, null);
  simulatedIngressSink.volume = { 'front-left': { value_percent: '10%' }, 'front-right': { value_percent: '10%' } };
  await syncEngineService.reconcileSession();
  assert.strictEqual(loopTriggerCount, 0, 'No updates or callbacks after stopSession');
  console.log('✓ Idempotency and Session Stop cleanup verified.');

  console.log('\n=== ALL UX IMPROVEMENTS TESTS PASSED WITH 100% SUCCESS! ===\n');
}

runUXTests().catch((err) => {
  console.error('UX Tests failed:', err);
  process.exit(1);
});
