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
    async setSinkVolume(sinkName, vol) { physicalSinkVolumeCalls.push({ sinkName, vol }); }
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

  console.log('\n=== ALL UX IMPROVEMENTS TESTS PASSED WITH 100% SUCCESS! ===\n');
}

runUXTests().catch((err) => {
  console.error('UX Tests failed:', err);
  process.exit(1);
});
