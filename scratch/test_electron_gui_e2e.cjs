const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const assert = require('node:assert');
const { createAudioBackend } = require('../electron/audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('../electron/audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('../electron/audio/speakerEngineService.cjs');

const audioBackend = createAudioBackend();
const streamRoutingService = new StreamRoutingService(audioBackend);
const speakerEngineService = new SpeakerEngineService(audioBackend);

ipcMain.handle('audio:list-output-devices', async () => audioBackend.listOutputDevicesWithStatus());
ipcMain.handle('audio:list-application-streams', async () => streamRoutingService.listApplicationStreams());
ipcMain.handle('audio:route-stream-to-all-speakers', async (_event, streamId) => streamRoutingService.routeToAllSpeakers(streamId));
ipcMain.handle('audio:restore-stream-output', async (_event, streamId) => streamRoutingService.restoreOriginalOutput(streamId));
ipcMain.handle('audio:move-stream-output', async (_event, streamId, targetSinkId) => streamRoutingService.moveStreamOutput(streamId, targetSinkId, speakerEngineService));
ipcMain.handle('audio:set-stream-volume', async (_event, streamId, volume) => audioBackend.setStreamVolume(streamId, volume));
ipcMain.handle('audio:set-stream-mute', async (_event, streamId, muted) => audioBackend.setStreamMute(streamId, muted));
ipcMain.handle('audio:set-sink-volume', async (_event, sinkName, volume) => audioBackend.setSinkVolume(sinkName, volume));
ipcMain.handle('audio:set-sink-mute', async (_event, sinkName, muted) => audioBackend.setSinkMute(sinkName, muted));
ipcMain.handle('audio:get-speaker-engine-status', async () => speakerEngineService.getStatus());
ipcMain.handle('audio:start-speaker-engine-session', async (_event, streamId, sinkIds) => speakerEngineService.startSession(streamId, sinkIds));
ipcMain.handle('audio:stop-speaker-engine-session', async () => speakerEngineService.stopSession());
ipcMain.handle('audio:disconnect-branch', async (_event, sinkId) => speakerEngineService.disconnectBranch(sinkId));
ipcMain.handle('audio:reconnect-branch', async (_event, sinkId) => speakerEngineService.reconnectBranch(sinkId));
ipcMain.handle('audio:set-master-volume', async (_event, volume) => speakerEngineService.setMasterVolume(volume));
ipcMain.handle('audio:set-master-mute', async (_event, muted) => speakerEngineService.setMasterMute(muted));
ipcMain.handle('audio:set-speaker-volume', async (_event, sinkId, volume) => speakerEngineService.setSpeakerVolume(sinkId, volume));
ipcMain.handle('audio:set-speaker-mute', async (_event, sinkId, muted) => speakerEngineService.setSpeakerMute(sinkId, muted));
ipcMain.handle('audio:get-wave-status', async () => speakerEngineService.getWaveStatus());
ipcMain.handle('audio:set-wave-config', async (_event, config) => speakerEngineService.setWaveConfig(config));
ipcMain.handle('audio:set-motion-config', async (_event, config) => speakerEngineService.setMotionConfig(config));

app.whenReady().then(async () => {
  console.log('=== Starting Electron GUI & IPC End-to-End Test ===\n');

  const win = new BrowserWindow({
    width: 920,
    height: 700,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, '..', 'electron', 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  // Register zoom shortcut handler exactly as in main.cjs
  win.webContents.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown') return;
    const isControlOrMeta = input.control || input.meta;
    if (!isControlOrMeta) return;

    if (input.key === '+' || input.key === '=' || input.code === 'Equal' || input.code === 'NumpadAdd') {
      const currentZoom = win.webContents.getZoomFactor();
      const nextZoom = Math.min(Math.round((currentZoom + 0.1) * 10) / 10, 3.0);
      win.webContents.setZoomFactor(nextZoom);
      event.preventDefault();
      return;
    }

    if (input.key === '-' || input.key === '_' || input.code === 'Minus' || input.code === 'NumpadSubtract') {
      const currentZoom = win.webContents.getZoomFactor();
      const nextZoom = Math.max(Math.round((currentZoom - 0.1) * 10) / 10, 0.5);
      win.webContents.setZoomFactor(nextZoom);
      event.preventDefault();
      return;
    }

    if (input.key === '0' || input.code === 'Digit0' || input.code === 'Numpad0') {
      win.webContents.setZoomFactor(1.0);
      event.preventDefault();
      return;
    }
  });

  await win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  console.log('✓ Successfully loaded dist/index.html with preload and context isolation');

  // Test 1: Test Zoom Shortcuts via Input Events
  console.log('\n1. Testing Electron Zoom Keyboard Shortcuts (Ctrl++, Ctrl+=, Ctrl+-, Ctrl+0)...');
  assert.strictEqual(win.webContents.getZoomFactor(), 1.0, 'Initial zoom must be 1.0');

  // Zoom in via Ctrl + =
  win.webContents.sendInputEvent({ type: 'keyDown', keyCode: '=', key: '=', code: 'Equal', modifiers: ['control'] });
  let zoom = win.webContents.getZoomFactor();
  console.log(`  Ctrl + = → Zoom Factor: ${zoom.toFixed(1)}`);
  assert.strictEqual(zoom, 1.1, 'Zoom must increase to 1.1');

  // Zoom in via Ctrl + +
  win.webContents.sendInputEvent({ type: 'keyDown', keyCode: '+', key: '+', code: 'Equal', modifiers: ['control'] });
  zoom = win.webContents.getZoomFactor();
  console.log(`  Ctrl + + → Zoom Factor: ${zoom.toFixed(1)}`);
  assert.strictEqual(zoom, 1.2, 'Zoom must increase to 1.2');

  // Zoom out via Ctrl + -
  win.webContents.sendInputEvent({ type: 'keyDown', keyCode: '-', key: '-', code: 'Minus', modifiers: ['control'] });
  zoom = win.webContents.getZoomFactor();
  console.log(`  Ctrl + - → Zoom Factor: ${zoom.toFixed(1)}`);
  assert.strictEqual(zoom, 1.1, 'Zoom must decrease to 1.1');

  // Reset zoom via Ctrl + 0
  win.webContents.sendInputEvent({ type: 'keyDown', keyCode: '0', key: '0', code: 'Digit0', modifiers: ['control'] });
  zoom = win.webContents.getZoomFactor();
  console.log(`  Ctrl + 0 → Zoom Factor: ${zoom.toFixed(1)}`);
  assert.strictEqual(zoom, 1.0, 'Zoom must reset to 1.0');

  // Test 2: Test Start Engine & Master Volume from Renderer Context
  console.log('\n2. Testing Renderer → IPC → Backend: Start Engine & Master Controls...');
  
  const result = await win.webContents.executeJavaScript(`
    (async () => {
      const streamsRes = await window.speakerFlow.listApplicationStreams();
      const devicesRes = await window.speakerFlow.listOutputDevices();
      const physicalSinks = devicesRes.filter(d => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers');
      
      const targetStream = streamsRes.streams.find(s => s.applicationName === 'Strawberry') || streamsRes.streams[0];
      if (!targetStream || physicalSinks.length < 2) {
        throw new Error('Strawberry stream or 2 physical sinks not found');
      }

      // Start session through preload window.speakerFlow
      await window.speakerFlow.startSpeakerEngineSession(targetStream.id, physicalSinks.map(s => s.id));
      const statusAfterStart = await window.speakerFlow.getSpeakerEngineStatus();

      // Set Master Volume to 50%
      const status50 = await window.speakerFlow.setMasterVolume(50);

      // Set Master Mute to true
      const statusMuted = await window.speakerFlow.setMasterMute(true);

      // Set Master Mute to false (unmute)
      const statusUnmuted = await window.speakerFlow.setMasterMute(false);

      // Stop Session
      await window.speakerFlow.stopSpeakerEngineSession();
      const statusAfterStop = await window.speakerFlow.getSpeakerEngineStatus();

      return {
        streamName: targetStream.applicationName,
        physicalCount: physicalSinks.length,
        sessionActiveAfterStart: statusAfterStart.session.active,
        branchesCount: statusAfterStart.session.branches.length,
        master50Volume: status50.masterVolume,
        master50BranchGains: status50.branchGains.map(b => b.gainPercent),
        masterMuted: statusMuted.masterMuted,
        masterMutedBranchGains: statusMuted.branchGains.map(b => b.gainPercent),
        masterUnmuted: statusUnmuted.masterMuted,
        masterUnmutedBranchGains: statusUnmuted.branchGains.map(b => b.gainPercent),
        sessionActiveAfterStop: statusAfterStop.session.active
      };
    })()
  `);

  console.log('  Renderer IPC Test Results:', result);
  assert.strictEqual(result.sessionActiveAfterStart, true, 'Session must start from renderer IPC');
  assert.strictEqual(result.branchesCount, 2, 'Must have 2 branches');
  assert.deepStrictEqual(result.master50BranchGains, [50, 50], 'Master 50% must set all branches to 50%');
  assert.strictEqual(result.masterMuted, true, 'Master muted must be true');
  assert.deepStrictEqual(result.masterMutedBranchGains, [0, 0], 'Master Mute must force all branches to 0%');
  assert.strictEqual(result.masterUnmuted, false, 'Master unmuted must be false');
  assert.deepStrictEqual(result.masterUnmutedBranchGains, [50, 50], 'Master Unmute must restore branches to 50%');
  assert.strictEqual(result.sessionActiveAfterStop, false, 'Session must stop cleanly from renderer IPC');

  console.log('\n=== ALL ELECTRON GUI & IPC TESTS PASSED 100%! ===\n');
  app.quit();
});
