const { app, BrowserWindow, ipcMain, globalShortcut } = require('electron');
const path = require('node:path');
const { createAudioBackend } = require('./audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('./audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('./audio/speakerEngineService.cjs');
const { createSystemPowerService } = require('./services/systemPowerService.cjs');

let mainWindow;
const audioBackend = createAudioBackend();
const streamRoutingService = new StreamRoutingService(audioBackend);
const speakerEngineService = new SpeakerEngineService(audioBackend);
const systemPowerService = createSystemPowerService();

async function adjustMasterVolume(delta) {
  if (!speakerEngineService.session) return;
  const currentStatus = await speakerEngineService.getWaveStatus();
  const currentVol = currentStatus?.masterVolume ?? 100;
  const nextVol = Math.max(0, Math.min(100, currentVol + delta));
  const updatedStatus = await speakerEngineService.setMasterVolume(nextVol);
  console.log(`[SpeakerFlow MediaKey] engine active: true`);
  console.log(`[SpeakerFlow MediaKey] master volume: ${currentVol} -> ${nextVol}`);
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('audio:master-volume-updated', {
      masterVolume: nextVol,
      masterMuted: updatedStatus?.masterMuted ?? currentStatus?.masterMuted ?? false
    });
  }
}

async function toggleMasterMute() {
  if (!speakerEngineService.session) return;
  const currentStatus = await speakerEngineService.getWaveStatus();
  const nextMute = !(currentStatus?.masterMuted ?? false);
  const updatedStatus = await speakerEngineService.setMasterMute(nextMute);
  console.log(`[SpeakerFlow MediaKey] engine active: true`);
  console.log(`[SpeakerFlow MediaKey] master mute: ${currentStatus?.masterMuted ?? false} -> ${nextMute}`);
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('audio:master-volume-updated', {
      masterVolume: updatedStatus?.masterVolume ?? currentStatus?.masterVolume ?? 100,
      masterMuted: nextMute
    });
  }
}

const lastMediaActionTime = {
  volumeUp: 0,
  volumeDown: 0,
  mute: 0
};
const MEDIA_DEBOUNCE_MS = 100;

async function handleVolumeUp() {
  const now = Date.now();
  if (now - lastMediaActionTime.volumeUp < MEDIA_DEBOUNCE_MS) return;
  lastMediaActionTime.volumeUp = now;
  await adjustMasterVolume(5);
}

async function handleVolumeDown() {
  const now = Date.now();
  if (now - lastMediaActionTime.volumeDown < MEDIA_DEBOUNCE_MS) return;
  lastMediaActionTime.volumeDown = now;
  await adjustMasterVolume(-5);
}

async function handleVolumeMute() {
  const now = Date.now();
  if (now - lastMediaActionTime.mute < MEDIA_DEBOUNCE_MS) return;
  lastMediaActionTime.mute = now;
  await toggleMasterMute();
}

function registerMediaShortcuts() {
  unregisterMediaShortcuts();
  if (!speakerEngineService.session) return;
  try {
    const upOk = globalShortcut.register('VolumeUp', () => {
      console.log('[SpeakerFlow MediaKey] global shortcut: VolumeUp');
      if (mainWindow && !mainWindow.isDestroyed() && mainWindow.isFocused()) return;
      handleVolumeUp().catch(() => {});
    });
    const downOk = globalShortcut.register('VolumeDown', () => {
      console.log('[SpeakerFlow MediaKey] global shortcut: VolumeDown');
      if (mainWindow && !mainWindow.isDestroyed() && mainWindow.isFocused()) return;
      handleVolumeDown().catch(() => {});
    });
    const muteOk = globalShortcut.register('VolumeMute', () => {
      console.log('[SpeakerFlow MediaKey] global shortcut: VolumeMute');
      if (mainWindow && !mainWindow.isDestroyed() && mainWindow.isFocused()) return;
      handleVolumeMute().catch(() => {});
    });
    if (!upOk || !downOk || !muteOk) {
      console.log(`[SpeakerFlow MediaKey] globalShortcut registration: VolumeUp=${upOk}, VolumeDown=${downOk}, VolumeMute=${muteOk} (expected on Wayland/GNOME where compositor owns media keys)`);
    }
  } catch (err) {
    console.log('[SpeakerFlow MediaKey] globalShortcut registration error:', err.message);
  }
}

function unregisterMediaShortcuts() {
  try {
    globalShortcut.unregister('VolumeUp');
    globalShortcut.unregister('VolumeDown');
    globalShortcut.unregister('VolumeMute');
  } catch {}
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 920,
    height: 700,
    minWidth: 650,
    minHeight: 500,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  mainWindow.on('closed', () => {
    unregisterMediaShortcuts();
    mainWindow = null;
  });

  // Media Keys & Native Zoom Shortcuts
  mainWindow.webContents.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown') return;

    // Media Keys for SpeakerFlow Engine (Active Session Only)
    if (speakerEngineService.session) {
      if (input.key === 'AudioVolumeUp' || input.code === 'AudioVolumeUp' || input.key === 'VolumeUp' || input.code === 'VolumeUp') {
        console.log(`[SpeakerFlow MediaKey] before-input-event: ${input.key || input.code}`);
        handleVolumeUp().catch(() => {});
        event.preventDefault();
        return;
      }
      if (input.key === 'AudioVolumeDown' || input.code === 'AudioVolumeDown' || input.key === 'VolumeDown' || input.code === 'VolumeDown') {
        console.log(`[SpeakerFlow MediaKey] before-input-event: ${input.key || input.code}`);
        handleVolumeDown().catch(() => {});
        event.preventDefault();
        return;
      }
      if (input.key === 'AudioVolumeMute' || input.code === 'AudioVolumeMute' || input.key === 'VolumeMute' || input.code === 'VolumeMute') {
        console.log(`[SpeakerFlow MediaKey] before-input-event: ${input.key || input.code}`);
        handleVolumeMute().catch(() => {});
        event.preventDefault();
        return;
      }
    }

    const isControlOrMeta = input.control || input.meta;
    if (!isControlOrMeta) return;

    // Zoom In: Ctrl + Plus / Equal / NumpadAdd
    if (input.key === '+' || input.key === '=' || input.code === 'Equal' || input.code === 'NumpadAdd') {
      const currentZoom = mainWindow.webContents.getZoomFactor();
      const nextZoom = Math.min(Math.round((currentZoom + 0.1) * 10) / 10, 3.0);
      mainWindow.webContents.setZoomFactor(nextZoom);
      event.preventDefault();
      return;
    }

    // Zoom Out: Ctrl + Minus / Underscore / NumpadSubtract
    if (input.key === '-' || input.key === '_' || input.code === 'Minus' || input.code === 'NumpadSubtract') {
      const currentZoom = mainWindow.webContents.getZoomFactor();
      const nextZoom = Math.max(Math.round((currentZoom - 0.1) * 10) / 10, 0.5);
      mainWindow.webContents.setZoomFactor(nextZoom);
      event.preventDefault();
      return;
    }

    // Reset Zoom: Ctrl + 0 / Numpad0
    if (input.key === '0' || input.code === 'Digit0' || input.code === 'Numpad0') {
      mainWindow.webContents.setZoomFactor(1.0);
      event.preventDefault();
      return;
    }
  });

  const devServerUrl = process.env.SPEAKERFLOW_DEV_SERVER_URL;
  if (devServerUrl) {
    mainWindow.loadURL(devServerUrl);
  } else {
    mainWindow.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  }
}

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
ipcMain.handle('audio:start-speaker-engine-session', async (_event, streamId, sinkIds) => {
  try {
    const result = await speakerEngineService.startSession(streamId, sinkIds);
    registerMediaShortcuts();
    return result;
  } catch (err) {
    unregisterMediaShortcuts();
    throw err;
  }
});
ipcMain.handle('audio:stop-speaker-engine-session', async () => {
  unregisterMediaShortcuts();
  return speakerEngineService.stopSession();
});
ipcMain.handle('audio:disconnect-branch', async (_event, sinkId) => speakerEngineService.disconnectBranch(sinkId));
ipcMain.handle('audio:reconnect-branch', async (_event, sinkId) => speakerEngineService.reconnectBranch(sinkId));
ipcMain.handle('audio:set-master-volume', async (_event, volume) => speakerEngineService.setMasterVolume(volume));
ipcMain.handle('audio:set-master-mute', async (_event, muted) => speakerEngineService.setMasterMute(muted));
ipcMain.handle('audio:set-speaker-volume', async (_event, sinkId, volume) => speakerEngineService.setSpeakerVolume(sinkId, volume));
ipcMain.handle('audio:set-speaker-mute', async (_event, sinkId, muted) => speakerEngineService.setSpeakerMute(sinkId, muted));
ipcMain.handle('audio:get-wave-status', async () => speakerEngineService.getWaveStatus());
ipcMain.handle('audio:set-wave-config', async (_event, config) => speakerEngineService.setWaveConfig(config));
ipcMain.handle('audio:set-motion-config', async (_event, config) => speakerEngineService.setMotionConfig(config));
ipcMain.handle('power:get-battery-status', async () => systemPowerService.getBatteryStatus());

app.whenReady().then(() => {
  createWindow();

  systemPowerService.startMonitoring((status) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('power:battery-updated', status);
    }
  });

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('before-quit', () => {
  unregisterMediaShortcuts();
  systemPowerService.stopMonitoring();
  speakerEngineService.stopSession().catch(() => {});
});

app.on('will-quit', () => {
  unregisterMediaShortcuts();
  systemPowerService.stopMonitoring();
});

app.on('window-all-closed', () => {
  unregisterMediaShortcuts();
  systemPowerService.stopMonitoring();
  speakerEngineService.stopSession().catch(() => {});
  if (process.platform !== 'darwin') app.quit();
});
