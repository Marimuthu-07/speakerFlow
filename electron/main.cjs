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

// Phase 2F.1-F (PART 6 & PART 7): Coalescing Coordinator for Core Audio events and renderer notifications
const audioCoalescingCoordinator = {
  timer: null,
  dirty: {
    devices: false,
    streams: false,
    engineStatus: false,
    reconcile: false,
    forceIngressReassert: false
  },
  clear() {
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.dirty = {
      devices: false,
      streams: false,
      engineStatus: false,
      reconcile: false,
      forceIngressReassert: false
    };
  },
  trigger({ devices = false, streams = false, engineStatus = false, reconcile = false, forceIngressReassert = false } = {}) {
    if (devices) this.dirty.devices = true;
    if (streams) this.dirty.streams = true;
    if (engineStatus) this.dirty.engineStatus = true;
    if (reconcile) this.dirty.reconcile = true;
    if (forceIngressReassert) this.dirty.forceIngressReassert = true;

    if (this.timer) {
      clearTimeout(this.timer);
    }

    // Trailing debounce window: 180ms coalesces device negotiation bursts into ONE reconciliation cycle
    this.timer = setTimeout(async () => {
      this.timer = null;
      const flags = { ...this.dirty };
      this.dirty = { devices: false, streams: false, engineStatus: false, reconcile: false, forceIngressReassert: false };

      if (flags.reconcile && speakerEngineService.session) {
        try {
          await speakerEngineService.reconcileSession({ forceIngressReassert: flags.forceIngressReassert });
          flags.engineStatus = true;
        } catch (err) {
          console.warn('[AudioCoordinator] reconcileSession error:', err.message);
        }
      }

      if (mainWindow && !mainWindow.isDestroyed()) {
        if (flags.devices) mainWindow.webContents.send('audio:devices-updated');
        if (flags.streams) mainWindow.webContents.send('audio:streams-updated');
        if (flags.engineStatus) mainWindow.webContents.send('audio:engine-status-updated');
      }
    }, 180);
  }
};

speakerEngineService.onMasterVolumeChanged = (data) => {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('audio:master-volume-updated', data);
  }
};

speakerEngineService.onSessionChanged = () => {
  if (!speakerEngineService.session) {
    unregisterEngineMasterShortcuts();
  }
  audioCoalescingCoordinator.trigger({ engineStatus: true });
};

audioBackend.on('devices-changed', () => {
  audioCoalescingCoordinator.trigger({
    devices: true,
    engineStatus: true,
    reconcile: Boolean(speakerEngineService.session),
    forceIngressReassert: true
  });
});

audioBackend.on('default-sink-changed', () => {
  audioCoalescingCoordinator.trigger({
    devices: true,
    engineStatus: true,
    reconcile: Boolean(speakerEngineService.session),
    forceIngressReassert: true
  });
});

audioBackend.on('streams-changed', () => {
  audioCoalescingCoordinator.trigger({
    streams: true,
    engineStatus: true,
    reconcile: Boolean(speakerEngineService.session)
  });
});

const systemPowerService = createSystemPowerService();

async function adjustMasterVolume(delta) {
  if (!speakerEngineService.session) return;
  const currentStatus = await speakerEngineService.getWaveStatus();
  const currentVol = currentStatus?.masterVolume ?? 100;
  const nextVol = Math.max(0, Math.min(100, currentVol + delta));
  const updatedStatus = await speakerEngineService.setMasterVolume(nextVol);
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

const ENGINE_MASTER_SHORTCUTS = [
  { accelerator: 'CommandOrControl+Alt+Up', action: handleVolumeUp },
  { accelerator: 'CommandOrControl+Alt+Down', action: handleVolumeDown },
  { accelerator: 'CommandOrControl+Alt+M', action: handleVolumeMute }
];

function registerEngineMasterShortcuts() {
  unregisterEngineMasterShortcuts();
  if (!speakerEngineService.session) return;

  for (const { accelerator, action } of ENGINE_MASTER_SHORTCUTS) {
    try {
      const ok = globalShortcut.register(accelerator, () => {
        action().catch((err) => console.error(`[Shortcuts] Error in ${accelerator} handler:`, err));
      });
      if (!ok) {
        console.warn(`[Shortcuts] Could not register ${accelerator}. Shortcut may be in use by another application.`);
      }
    } catch (err) {
      console.warn(`[Shortcuts] Failed to register ${accelerator}:`, err.message);
    }
  }

  // Also retain best-effort media key registration for external keyboards
  try {
    globalShortcut.register('VolumeUp', () => {
      if (mainWindow && !mainWindow.isDestroyed() && mainWindow.isFocused()) return;
      handleVolumeUp().catch(() => {});
    });
    globalShortcut.register('VolumeDown', () => {
      if (mainWindow && !mainWindow.isDestroyed() && mainWindow.isFocused()) return;
      handleVolumeDown().catch(() => {});
    });
    globalShortcut.register('VolumeMute', () => {
      if (mainWindow && !mainWindow.isDestroyed() && mainWindow.isFocused()) return;
      handleVolumeMute().catch(() => {});
    });
  } catch {}
}

function unregisterEngineMasterShortcuts() {
  for (const { accelerator } of ENGINE_MASTER_SHORTCUTS) {
    try {
      if (globalShortcut.isRegistered(accelerator)) {
        globalShortcut.unregister(accelerator);
      }
    } catch (err) {
      console.warn(`[Shortcuts] Failed to unregister ${accelerator}:`, err.message);
    }
  }
  try {
    globalShortcut.unregister('VolumeUp');
    globalShortcut.unregister('VolumeDown');
    globalShortcut.unregister('VolumeMute');
  } catch {}
}

function registerMediaShortcuts() {
  registerEngineMasterShortcuts();
}

function unregisterMediaShortcuts() {
  unregisterEngineMasterShortcuts();
}

let isShuttingDown = false;
async function performGracefulShutdown() {
  if (isShuttingDown) return;
  isShuttingDown = true;

  audioCoalescingCoordinator.clear();
  unregisterEngineMasterShortcuts();
  try {
    globalShortcut.unregisterAll();
  } catch {}

  systemPowerService.stopMonitoring();

  if (speakerEngineService.session) {
    try {
      await speakerEngineService.stopSession();
    } catch (err) {
      console.warn('[Main] Error stopping session on shutdown:', err.message);
    }
  }

  try {
    audioBackend.stopMonitoring();
  } catch {}

  try {
    audioBackend.destroy();
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

  mainWindow.on('close', async (e) => {
    if (mainWindow && !mainWindow.isDestroyed() && !isShuttingDown) {
      e.preventDefault();
      try {
        await Promise.race([
          performGracefulShutdown(),
          new Promise((resolve) => setTimeout(resolve, 3000))
        ]);
      } catch (err) {
        console.warn('[Main] Error during shutdown race:', err);
      }
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.destroy();
      }
      mainWindow = null;
      app.quit();
    }
  });

  mainWindow.on('closed', () => {
    unregisterEngineMasterShortcuts();
    mainWindow = null;
  });

  // Media Keys & Native Zoom Shortcuts
  mainWindow.webContents.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown') return;

    // Media & Master Shortcuts for SpeakerFlow Engine (Active Session Only)
    if (speakerEngineService.session) {
      const isControlOrMeta = Boolean(input.control || input.meta);
      const isAlt = Boolean(input.alt);

      // Ctrl + Alt + ArrowUp / ArrowDown / M
      if (isControlOrMeta && isAlt) {
        if (input.key === 'ArrowUp' || input.code === 'ArrowUp' || input.key === 'Up') {
          handleVolumeUp().catch(() => {});
          event.preventDefault();
          return;
        }
        if (input.key === 'ArrowDown' || input.code === 'ArrowDown' || input.key === 'Down') {
          handleVolumeDown().catch(() => {});
          event.preventDefault();
          return;
        }
        if (input.key === 'm' || input.key === 'M' || input.code === 'KeyM') {
          handleVolumeMute().catch(() => {});
          event.preventDefault();
          return;
        }
      }

      if (input.key === 'AudioVolumeUp' || input.code === 'AudioVolumeUp' || input.key === 'VolumeUp' || input.code === 'VolumeUp') {
        handleVolumeUp().catch(() => {});
        event.preventDefault();
        return;
      }
      if (input.key === 'AudioVolumeDown' || input.code === 'AudioVolumeDown' || input.key === 'VolumeDown' || input.code === 'VolumeDown') {
        handleVolumeDown().catch(() => {});
        event.preventDefault();
        return;
      }
      if (input.key === 'AudioVolumeMute' || input.code === 'AudioVolumeMute' || input.key === 'VolumeMute' || input.code === 'VolumeMute') {
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
ipcMain.handle('audio:start-speaker-engine-session', async (_event, streamId, sinkIds, isManual = false) => {
  try {
    const result = await speakerEngineService.startSession(streamId, sinkIds, isManual);
    registerEngineMasterShortcuts();
    return result;
  } catch (err) {
    unregisterEngineMasterShortcuts();
    throw err;
  }
});
ipcMain.handle('audio:set-session-stream', async (_event, streamId, isManual) => speakerEngineService.setSessionStream(streamId, isManual));
ipcMain.handle('audio:stop-speaker-engine-session', async () => {
  unregisterEngineMasterShortcuts();
  return speakerEngineService.stopSession();
});
ipcMain.handle('audio:disconnect-branch', async (_event, sinkId) => speakerEngineService.disconnectBranch(sinkId));
ipcMain.handle('audio:reconnect-branch', async (_event, sinkId) => speakerEngineService.reconnectBranch(sinkId));
ipcMain.handle('audio:set-master-volume', async (_event, volume) => speakerEngineService.setMasterVolume(volume));
ipcMain.handle('audio:set-master-mute', async (_event, muted) => speakerEngineService.setMasterMute(muted));
ipcMain.handle('audio:set-speaker-volume', async (_event, sinkId, volume) => speakerEngineService.setSpeakerVolume(sinkId, volume));
ipcMain.handle('audio:set-speaker-mute', async (_event, sinkId, muted) => speakerEngineService.setSpeakerMute(sinkId, muted));
ipcMain.handle('audio:set-speaker-delay', async (_event, sinkId, delayMs) => speakerEngineService.setSpeakerDelay(sinkId, delayMs));
ipcMain.handle('audio:get-wave-status', async () => speakerEngineService.getWaveStatus());
ipcMain.handle('audio:set-wave-config', async (_event, config) => speakerEngineService.setWaveConfig(config));
ipcMain.handle('audio:set-motion-config', async (_event, config) => speakerEngineService.setMotionConfig(config));
ipcMain.handle('power:get-battery-status', async () => systemPowerService.getBatteryStatus());

app.whenReady().then(() => {
  createWindow();
  audioBackend.startMonitoring();

  systemPowerService.startMonitoring((status) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('power:battery-updated', status);
    }
  });

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('before-quit', (e) => {
  if (!isShuttingDown) {
    e.preventDefault();
    performGracefulShutdown().finally(() => {
      app.quit();
    });
  }
});

app.on('will-quit', () => {
  try {
    globalShortcut.unregisterAll();
  } catch {}
});

app.on('window-all-closed', async () => {
  await performGracefulShutdown();
  if (process.platform !== 'darwin') app.quit();
});
