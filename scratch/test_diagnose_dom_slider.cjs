const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
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

app.whenReady().then(async () => {
  console.log('╔══════════════════════════════════════════════════════════════════════════════╗');
  console.log('║       SpeakerFlow REACT UI DOM SLIDER Live Diagnostic Tracing Test           ║');
  console.log('╚══════════════════════════════════════════════════════════════════════════════╝\n');

  const audioBackend = createAudioBackend();
  const streamRoutingService = new StreamRoutingService(audioBackend);
  const speakerEngineService = new SpeakerEngineService(audioBackend);

  let ipcCalls = [];

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
  
  ipcMain.handle('audio:set-master-volume', async (_event, volume) => {
    ipcCalls.push({ name: 'setMasterVolume', volume, time: Date.now() });
    console.log(`  [IPC Handler] 'audio:set-master-volume' called with: ${volume}`);
    return speakerEngineService.setMasterVolume(volume);
  });
  
  ipcMain.handle('audio:set-master-mute', async (_event, muted) => speakerEngineService.setMasterMute(muted));
  ipcMain.handle('audio:set-speaker-volume', async (_event, sinkId, volume) => speakerEngineService.setSpeakerVolume(sinkId, volume));
  ipcMain.handle('audio:set-speaker-mute', async (_event, sinkId, muted) => speakerEngineService.setSpeakerMute(sinkId, muted));
  ipcMain.handle('audio:get-wave-status', async () => speakerEngineService.getWaveStatus());
  ipcMain.handle('audio:set-wave-config', async (_event, config) => speakerEngineService.setWaveConfig(config));
  ipcMain.handle('audio:set-motion-config', async (_event, config) => speakerEngineService.setMotionConfig(config));

  const win = new BrowserWindow({
    width: 1000,
    height: 800,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, '..', 'electron', 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  win.webContents.on('console-message', (event, level, message) => {
    console.log(`  [Browser Console] ${message}`);
  });

  await win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  await new Promise(r => setTimeout(r, 600));

  try {
    // 1. Check if React mounted and Master slider exists in DOM
    const sliderInfo = await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        if (!slider) return { found: false };
        return {
          found: true,
          tagName: slider.tagName,
          min: slider.min,
          max: slider.max,
          value: slider.value,
          className: slider.className,
          disabled: slider.disabled
        };
      })()
    `);
    console.log('DOM Master Slider Info:', sliderInfo);

    // 2. Start session with Strawberry
    const streams = await audioBackend.listApplicationStreams();
    const strawberry = streams.find(s => s.applicationName === 'Strawberry') || streams[0];
    const allDevices = await audioBackend.listOutputDevicesWithStatus();
    const physicalSinks = allDevices.filter(d => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers');

    console.log(`\n--- Starting Session with Stream ${strawberry.applicationName} (${strawberry.id}) and ${physicalSinks.length} speakers ---`);
    await speakerEngineService.startSession(strawberry.id, physicalSinks.map(s => s.id));
    await new Promise(r => setTimeout(r, 600));

    // 3. Simulate user physically sliding the React Master Slider in the DOM (dispatching React input event)
    console.log('\n--- Simulating User Moving Master Slider in React UI: 100% → 50% ---');
    ipcCalls = [];
    await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        if (!slider) throw new Error('Slider not found');
        
        // Use native value setter to trigger React 16+ onChange handler
        const nativeInputValueSetter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
        nativeInputValueSetter.call(slider, '50');
        
        const ev = new Event('input', { bubbles: true });
        slider.dispatchEvent(ev);
        const changeEv = new Event('change', { bubbles: true });
        slider.dispatchEvent(changeEv);
        return slider.value;
      })()
    `);

    await new Promise(r => setTimeout(r, 300));
    console.log(`IPC Calls logged:`, ipcCalls);

    // Check PipeWire volumes
    let pwSinkInputs = await getActualPipeWireSinkInputs();
    let wave = speakerEngineService.waveController;
    console.log(`Controller masterVolume: ${wave.masterVolume}%`);
    for (const s of physicalSinks) {
      const branchState = wave.branches.get(s.id);
      const match = branchState?.branchSinkId ? findBranchSinkInput(pwSinkInputs, branchState.branchSinkId) : null;
      console.log(`Speaker ${s.name}: branch sinkInputId=${branchState?.sinkInputId}, PipeWire vol=${match ? parseVolumePercent(match.volume) : 'none'}%`);
    }

    // 4. Simulate user moving Master Slider: 50% → 20%
    console.log('\n--- Simulating User Moving Master Slider in React UI: 50% → 20% ---');
    ipcCalls = [];
    await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        const nativeInputValueSetter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
        nativeInputValueSetter.call(slider, '20');
        slider.dispatchEvent(new Event('input', { bubbles: true }));
        slider.dispatchEvent(new Event('change', { bubbles: true }));
        return slider.value;
      })()
    `);

    await new Promise(r => setTimeout(r, 300));
    console.log(`IPC Calls logged:`, ipcCalls);

    pwSinkInputs = await getActualPipeWireSinkInputs();
    console.log(`Controller masterVolume: ${wave.masterVolume}%`);
    for (const s of physicalSinks) {
      const branchState = wave.branches.get(s.id);
      const match = branchState?.branchSinkId ? findBranchSinkInput(pwSinkInputs, branchState.branchSinkId) : null;
      console.log(`Speaker ${s.name}: branch sinkInputId=${branchState?.sinkInputId}, PipeWire vol=${match ? parseVolumePercent(match.volume) : 'none'}%`);
    }

    // 5. Clean up
    console.log('\n--- Stopping Session ---');
    await speakerEngineService.stopSession();
    console.log('✓ Done');

  } catch (err) {
    console.error('Test error:', err);
    try {
      await speakerEngineService.stopSession();
    } catch {}
  } finally {
    app.quit();
  }
});
