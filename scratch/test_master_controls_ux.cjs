const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const assert = require('node:assert');

const { createAudioBackend } = require('../electron/audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('../electron/audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('../electron/audio/speakerEngineService.cjs');

app.whenReady().then(async () => {
  console.log('╔══════════════════════════════════════════════════════════════════════════════╗');
  console.log('║       Master Output UI State & Lifecycle End-to-End Regression Test          ║');
  console.log('╚══════════════════════════════════════════════════════════════════════════════╝\n');

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

  const win = new BrowserWindow({
    width: 950,
    height: 750,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, '..', 'electron', 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  win.webContents.on('console-message', (_e, level, msg) => {
    console.log(`  [DOM Log] ${msg}`);
  });

  await win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  await new Promise((r) => setTimeout(r, 800));

  try {
    // 1. Check IDLE State
    console.log('1. Checking IDLE State UI Controls...');
    const idleState = await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        const muteBtn = document.querySelector('.mute-toggle-btn');
        const statusPill = document.querySelector('.master-status-pill');
        const badge = document.querySelector('.master-engine-badge');
        const volBadge = document.querySelector('.vol-badge');
        return {
          sliderDisabled: slider?.disabled,
          sliderValue: slider?.value,
          muteBtnDisabled: muteBtn?.disabled,
          statusPillText: statusPill?.textContent?.trim(),
          badgeText: badge?.textContent?.trim(),
          volBadgeText: volBadge?.textContent?.trim()
        };
      })()
    `);

    console.log('  IDLE UI State:', idleState);
    assert.strictEqual(idleState.sliderDisabled, true, 'Master slider must be disabled when Engine is IDLE');
    assert.strictEqual(idleState.muteBtnDisabled, true, 'Mute All button must be disabled when Engine is IDLE');
    assert.strictEqual(idleState.statusPillText, 'Engine Inactive', 'Status pill must indicate Engine Inactive');
    assert(idleState.volBadgeText.includes('Inactive'), 'Volume badge should show Inactive indicator');
    console.log('  ✓ IDLE state verified: Master controls properly disabled with inactive indicator.');

    // 2. Switch to Engine Graph Tab & Click "Start Engine" in UI
    console.log('\n2. Navigating to Engine Graph Tab & Clicking "Start Engine" Button in UI...');
    await win.webContents.executeJavaScript(`
      (() => {
        const tabBtns = Array.from(document.querySelectorAll('.nav-tab-btn'));
        const topologyTab = tabBtns.find(btn => btn.textContent.includes('Engine Graph'));
        if (topologyTab) topologyTab.click();
      })()
    `);
    await new Promise((r) => setTimeout(r, 400));

    const btnInfo = await win.webContents.executeJavaScript(`
      (() => {
        const startBtn = Array.from(document.querySelectorAll('button')).find(b => b.textContent.includes('Start Engine'));
        return {
          found: Boolean(startBtn),
          text: startBtn?.textContent,
          disabled: startBtn?.disabled
        };
      })()
    `);
    console.log('  Start Engine button status before click:', btnInfo);

    await win.webContents.executeJavaScript(`
      (() => {
        const startBtn = Array.from(document.querySelectorAll('button')).find(b => b.textContent.includes('Start Engine'));
        if (startBtn && !startBtn.disabled) startBtn.click();
      })()
    `);

    // Wait for session to start and polling / refresh to complete
    await new Promise((r) => setTimeout(r, 2000));

    const activeState = await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        const muteBtn = document.querySelector('.mute-toggle-btn');
        const statusPill = document.querySelector('.master-status-pill');
        const badge = document.querySelector('.master-engine-badge');
        const volBadge = document.querySelector('.vol-badge');
        return {
          sliderDisabled: slider?.disabled,
          sliderValue: slider?.value,
          muteBtnDisabled: muteBtn?.disabled,
          statusPillText: statusPill?.textContent?.trim(),
          badgeText: badge?.textContent?.trim(),
          volBadgeText: volBadge?.textContent?.trim()
        };
      })()
    `);

    console.log('  ACTIVE UI State:', activeState);
    assert.strictEqual(activeState.sliderDisabled, false, 'Master slider must be enabled when Engine is ACTIVE');
    assert.strictEqual(activeState.muteBtnDisabled, false, 'Mute All button must be enabled when Engine is ACTIVE');
    assert.strictEqual(activeState.statusPillText, 'Engine Active', 'Status pill must indicate Engine Active');
    console.log('  ✓ ACTIVE state verified: Master controls enabled.');

    // 3. Change Master to 65% in ACTIVE state
    console.log('\n3. Adjusting Master Volume to 65% while Engine is ACTIVE...');
    await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
        setter.call(slider, '65');
        slider.dispatchEvent(new Event('input', { bubbles: true }));
        slider.dispatchEvent(new Event('change', { bubbles: true }));
      })()
    `);
    await new Promise((r) => setTimeout(r, 400));

    const afterAdjustState = await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        return slider?.value;
      })()
    `);
    assert.strictEqual(afterAdjustState, '65', 'Master slider value should be 65%');
    assert.strictEqual(speakerEngineService.waveController.masterVolume, 65, 'Backend master volume should be 65%');
    console.log('  ✓ Value changed to 65% on backend and frontend.');

    // 4. Click "Stop Engine" in UI and Check STOP State
    console.log('\n4. Clicking "Stop Engine" in UI and Checking State Preservation...');
    await win.webContents.executeJavaScript(`
      (() => {
        const stopBtn = Array.from(document.querySelectorAll('button')).find(b => b.textContent.includes('Stop Engine'));
        if (stopBtn && !stopBtn.disabled) stopBtn.click();
      })()
    `);
    await new Promise((r) => setTimeout(r, 2000));

    const stoppedState = await win.webContents.executeJavaScript(`
      (() => {
        const slider = document.querySelector('.master-slider');
        const muteBtn = document.querySelector('.mute-toggle-btn');
        const statusPill = document.querySelector('.master-status-pill');
        const volBadge = document.querySelector('.vol-badge');
        return {
          sliderDisabled: slider?.disabled,
          sliderValue: slider?.value,
          muteBtnDisabled: muteBtn?.disabled,
          statusPillText: statusPill?.textContent?.trim(),
          volBadgeText: volBadge?.textContent?.trim()
        };
      })()
    `);

    console.log('  STOPPED UI State:', stoppedState);
    assert.strictEqual(stoppedState.sliderDisabled, true, 'Master slider must be disabled when Engine is stopped');
    assert.strictEqual(stoppedState.muteBtnDisabled, true, 'Mute All button must be disabled when Engine is stopped');
    assert.strictEqual(stoppedState.sliderValue, '65', 'Master volume value must be PRESERVED (65%) when stopped');
    assert.strictEqual(speakerEngineService.waveController.masterVolume, 65, 'Backend master volume must be preserved');
    console.log('  ✓ STOP state verified: Controls disabled, value 65% preserved.');

    console.log('\n=== ALL MASTER OUTPUT UX REGRESSION TESTS PASSED! ===\n');
  } catch (err) {
    console.error('Test error:', err);
    try {
      await speakerEngineService.stopSession();
    } catch {}
    process.exit(1);
  } finally {
    app.quit();
  }
});
