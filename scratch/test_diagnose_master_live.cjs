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
  console.log('║       SpeakerFlow MASTER OUTPUT Live Diagnostic Tracing Trace Suite          ║');
  console.log('╚══════════════════════════════════════════════════════════════════════════════╝\n');

  const audioBackend = createAudioBackend();
  const streamRoutingService = new StreamRoutingService(audioBackend);
  const speakerEngineService = new SpeakerEngineService(audioBackend);

  // Hook instrumentation logging into main IPC handlers
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
  
  let lastIpcReceivedMaster = null;
  ipcMain.handle('audio:set-master-volume', async (_event, volume) => {
    lastIpcReceivedMaster = volume;
    console.log(`\n  [Layer 2 - IPC Main Handler] Received 'audio:set-master-volume' volume = ${volume}`);
    return speakerEngineService.setMasterVolume(volume);
  });
  
  ipcMain.handle('audio:set-master-mute', async (_event, muted) => speakerEngineService.setMasterMute(muted));
  ipcMain.handle('audio:set-speaker-volume', async (_event, sinkId, volume) => speakerEngineService.setSpeakerVolume(sinkId, volume));
  ipcMain.handle('audio:set-speaker-mute', async (_event, sinkId, muted) => speakerEngineService.setSpeakerMute(sinkId, muted));
  ipcMain.handle('audio:get-wave-status', async () => speakerEngineService.getWaveStatus());
  ipcMain.handle('audio:set-wave-config', async (_event, config) => speakerEngineService.setWaveConfig(config));
  ipcMain.handle('audio:set-motion-config', async (_event, config) => speakerEngineService.setMotionConfig(config));

  // Instrument WaveEngineController methods for detailed logging
  const originalSetMasterVolume = speakerEngineService.waveController.setMasterVolume.bind(speakerEngineService.waveController);
  speakerEngineService.waveController.setMasterVolume = async function(volume) {
    console.log(`  [Layer 3 - WaveEngineController.setMasterVolume()] Setting masterVolume = ${volume}`);
    const res = await originalSetMasterVolume(volume);
    console.log(`  [Layer 3 - WaveEngineController.setMasterVolume()] controller masterGain state is now = ${this.masterVolume}%`);
    return res;
  };

  const originalCalc = speakerEngineService.waveController.calculateBranchTargetGain.bind(speakerEngineService.waveController);
  speakerEngineService.waveController.calculateBranchTargetGain = function(sinkId, dynamicEngineGain = 1.0) {
    const res = originalCalc(sinkId, dynamicEngineGain);
    return res;
  };

  const pactlCommandsLogged = [];
  const originalApplyBranchVolume = speakerEngineService.waveController.applyBranchVolume.bind(speakerEngineService.waveController);
  speakerEngineService.waveController.applyBranchVolume = function(branch) {
    const dynGain = this.enabled ? (branch.targetGain ?? 1.0) : 1.0;
    const { targetGain, targetPercent } = this.calculateBranchTargetGain(branch.sinkId, dynGain);
    pactlCommandsLogged.push({
      sinkId: branch.sinkId,
      sinkName: branch.sinkName,
      branchSinkId: branch.branchSinkId,
      sinkInputId: branch.sinkInputId,
      dynamicGain: dynGain,
      targetGain,
      targetPercent,
      command: `pactl set-sink-input-volume ${branch.sinkInputId} ${targetPercent}%`
    });
    return originalApplyBranchVolume(branch);
  };

  // Load GUI window
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

  await win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  console.log('✓ Loaded GUI with preload and IPC ready\n');

  try {
    // 1. Find Strawberry stream and physical sinks
    const streams = await audioBackend.listApplicationStreams();
    const strawberry = streams.find(s => s.applicationName === 'Strawberry') || streams[0];
    if (!strawberry) {
      throw new Error('Strawberry stream not found');
    }
    const allDevices = await audioBackend.listOutputDevicesWithStatus();
    const physicalSinks = allDevices.filter(d => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers');
    if (physicalSinks.length < 2) {
      throw new Error(`Expected at least 2 physical sinks, found ${physicalSinks.length}`);
    }

    console.log(`Target Stream: ${strawberry.applicationName} (ID: ${strawberry.id}, sink: ${strawberry.currentSinkId})`);
    console.log(`Physical Speakers:`);
    physicalSinks.forEach((s, idx) => console.log(`  Branch ${String.fromCharCode(65 + idx)}: ${s.name} (${s.id})`));

    // Start Engine Session from GUI/Renderer Context
    console.log('\n--- Starting Engine Session via window.speakerFlow.startSpeakerEngineSession ---');
    await win.webContents.executeJavaScript(`
      window.speakerFlow.startSpeakerEngineSession(${strawberry.id}, ${JSON.stringify(physicalSinks.map(s => s.id))})
    `);

    // Wait a brief moment for session stabilization
    await new Promise(r => setTimeout(r, 500));
    let status = await speakerEngineService.getStatus();
    console.log(`Engine Status: active=${status.session?.active}, state=${status.session?.state}, branches=${status.session?.branches?.length}`);

    async function printDiagnosticLayerSnapshot(stepLabel, requestedMasterVal) {
      console.log(`\n================================================================================`);
      console.log(`STEP: ${stepLabel}`);
      console.log(`================================================================================`);
      
      // Drain recent pactl commands log
      const recentPactl = pactlCommandsLogged.splice(0, pactlCommandsLogged.length);

      const pwSinkInputs = await getActualPipeWireSinkInputs();
      const wave = speakerEngineService.waveController;
      const waveStatus = wave.getStatus();

      console.log(`UI requested master        = ${requestedMasterVal}%`);
      console.log(`IPC received master         = ${lastIpcReceivedMaster}%`);
      console.log(`controller masterGain       = ${wave.masterVolume}% (masterMuted=${wave.masterMuted})`);

      for (let i = 0; i < physicalSinks.length; i++) {
        const s = physicalSinks[i];
        const letter = String.fromCharCode(65 + i);
        const branchState = wave.branches.get(s.id);
        const userGain = wave.userSpeakerVolumes.get(s.id) ?? 100;
        const dynGain = wave.enabled ? (branchState?.targetGain ?? 1.0) : 1.0;
        const { targetPercent } = wave.calculateBranchTargetGain(s.id, dynGain);
        const match = branchState?.branchSinkId ? findBranchSinkInput(pwSinkInputs, branchState.branchSinkId) : null;
        const actualPwVol = match ? parseVolumePercent(match.volume) : 'NOT FOUND';

        console.log(`--- Branch ${letter} (${s.name}) ---`);
        console.log(`  speaker userGain          = ${userGain}% (muted=${wave.userSpeakerMuted.get(s.id) ?? false})`);
        console.log(`  speaker dynamicGain       = ${Math.round(dynGain * 100)}%`);
        console.log(`  branch sinkInputId        = ${branchState?.sinkInputId ?? 'null'} (PW index: ${match?.index ?? 'none'})`);
        console.log(`  calculated target         = ${targetPercent}%`);
        console.log(`  actual PipeWire volume    = ${actualPwVol}%`);
      }

      if (recentPactl.length > 0) {
        console.log(`Pactl commands executed in this transition:`);
        recentPactl.forEach(p => console.log(`  > ${p.command} (for ${p.sinkName})`));
      } else {
        console.log(`Pactl commands executed in this transition: (None)`);
      }
    }

    // 1. Record current state (100% Master, Engine ON, Motion OFF)
    await printDiagnosticLayerSnapshot('1. Record Initial State (Master = 100%, Motion = OFF)', 100);

    // 2. Move Master 100% → 50% via GUI Renderer
    console.log('\n>>> Executing in Renderer: window.speakerFlow.setMasterVolume(50)');
    await win.webContents.executeJavaScript(`window.speakerFlow.setMasterVolume(50)`);
    await new Promise(r => setTimeout(r, 200));
    await printDiagnosticLayerSnapshot('2. Move Master from 100% → 50%', 50);

    // 3. Move Master 50% → 20% via GUI Renderer
    console.log('\n>>> Executing in Renderer: window.speakerFlow.setMasterVolume(20)');
    await win.webContents.executeJavaScript(`window.speakerFlow.setMasterVolume(20)`);
    await new Promise(r => setTimeout(r, 200));
    await printDiagnosticLayerSnapshot('3. Move Master from 50% → 20%', 20);

    // Reset Master to 100% before motion test
    console.log('\n>>> Resetting Master to 100%');
    await win.webContents.executeJavaScript(`window.speakerFlow.setMasterVolume(100)`);
    await new Promise(r => setTimeout(r, 200));

    // 4. Enable Motion mode (smooth) and repeat 100% → 50%
    console.log('\n>>> Enabling Motion Engine (mode: smooth, enabled: true)');
    await win.webContents.executeJavaScript(`
      (async () => {
        await window.speakerFlow.setMotionConfig({ motionMode: 'smooth', motionSpeed: 0.5 });
        await window.speakerFlow.setWaveConfig({ enabled: true });
      })()
    `);
    await new Promise(r => setTimeout(r, 500)); // Let motion scheduler run several ticks

    await printDiagnosticLayerSnapshot('4a. Motion Mode Active (Master = 100%)', 100);

    console.log('\n>>> Executing in Renderer during Motion Mode: window.speakerFlow.setMasterVolume(50)');
    await win.webContents.executeJavaScript(`window.speakerFlow.setMasterVolume(50)`);
    await new Promise(r => setTimeout(r, 200)); // Let motion scheduler run with 50% master
    await printDiagnosticLayerSnapshot('4b. Motion Mode Active (Master = 50%)', 50);

    // Stop Engine Session and clean up
    console.log('\n--- Stopping Engine Session and Restoring Stream ---');
    await win.webContents.executeJavaScript(`window.speakerFlow.stopSpeakerEngineSession()`);
    await new Promise(r => setTimeout(r, 400));
    console.log('✓ Engine session stopped successfully');

  } catch (err) {
    console.error('Diagnostic error:', err);
    try {
      await speakerEngineService.stopSession();
    } catch {}
  } finally {
    app.quit();
  }
});
