const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const { createAudioBackend } = require('./audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('./audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('./audio/speakerEngineService.cjs');

let mainWindow;
const audioBackend = createAudioBackend();
const streamRoutingService = new StreamRoutingService(audioBackend);
const speakerEngineService = new SpeakerEngineService(audioBackend);

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

  // Electron Native Zoom Shortcuts: Ctrl/Cmd + (+/=/-/0)
  mainWindow.webContents.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown') return;

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

app.whenReady().then(() => {
  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  speakerEngineService.stopSession().catch(() => {});
  if (process.platform !== 'darwin') app.quit();
});
