const { randomUUID } = require('node:crypto');
const { spawn } = require('node:child_process');
const { WaveEngineController } = require('./waveEngineController.cjs');

class SpeakerEngineService {
  constructor(audioBackend) {
    this.audioBackend = audioBackend;
    this.session = null;
    this.lastMessage = '';
    this.waveController = new WaveEngineController(audioBackend);
  }

  async getStatus() {
    await this.reconcileSession();
    const allOutputs = await this.audioBackend.listOutputDevicesWithStatus();
    const outputs = allOutputs.filter(
      (device) => !device.isVirtual && !device.id.startsWith('speakerflow.session.') && device.id !== 'all_speakers'
    );

    const waveStatus = this.waveController.getStatus();

    return {
      outputs,
      session: this.session
        ? {
            active: true,
            state: this.session.state,
            streamId: this.session.streamId,
            streamName: this.session.streamName,
            originalSinkId: this.session.originalSinkId,
            originalSinkName: this.session.originalSinkName,
            ingressSinkId: this.session.ingressSinkId,
            ingressStatus: this.session.ingressStatus,
            tapSourceId: this.session.tapSourceId,
            tapStatus: this.session.tapStatus,
            branches: this.session.branches.map((b) => ({
              sinkId: b.sinkId,
              sinkName: b.sinkName,
              branchSinkId: b.branchSinkId,
              state: b.state,
              error: b.error || null
            })),
            activeBranchCount: this.session.branches.filter((b) => b.state === 'active').length,
            totalBranchCount: this.session.branches.length,
            message: this.lastMessage,
            wave: waveStatus
          }
        : { active: false, state: 'idle', message: this.lastMessage, wave: waveStatus }
    };
  }

  async startSession(streamId, selectedSinkIds) {
    if (this.session) {
      throw new Error('A Speaker Engine session is already active.');
    }

    const sinkIdList = Array.isArray(selectedSinkIds)
      ? selectedSinkIds
      : [selectedSinkIds].filter(Boolean);

    if (sinkIdList.length === 0) {
      throw new Error('At least one physical output must be selected to start the engine.');
    }

    const [stream, allSinks] = await Promise.all([
      this.findApplicationStream(streamId),
      this.audioBackend.listSinks()
    ]);

    if (!stream) {
      throw new Error('The selected application stream is no longer available.');
    }

    const validPhysicalSinks = [];
    for (const sinkId of sinkIdList) {
      const sink = allSinks.find((item) => item.name === sinkId);
      if (sink && sink.properties?.['node.virtual'] !== 'true' && sink.name !== 'all_speakers') {
        validPhysicalSinks.push({
          id: sink.name,
          name: sink.description || sink.name
        });
      }
    }

    if (validPhysicalSinks.length === 0) {
      throw new Error('None of the selected outputs are currently available physical sinks.');
    }

    const sessionId = randomUUID();
    const ingressSinkId = `speakerflow.session.${sessionId}.ingress`;
    const tapSourceId = `speakerflow.session.${sessionId}.tap`;

    const ingressProcess = this.startIngressTap(ingressSinkId, tapSourceId);

    const session = {
      sessionId,
      ingressProcess,
      ingressSinkId,
      tapSourceId,
      ingressStatus: 'starting',
      tapStatus: 'starting',
      state: 'starting',
      streamId: stream.id,
      streamName: stream.applicationName,
      originalSinkId: stream.currentSinkId,
      originalSinkName: stream.currentSinkName,
      branches: [],
      stopping: false
    };
    this.session = session;
    this.lastMessage = '';

    ingressProcess.once('error', (error) => {
      if (this.session === session) {
        session.ingressStatus = 'error';
        session.tapStatus = 'error';
        this.lastMessage = `The temporary Speaker Engine ingress could not start: ${error.message}`;
        this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
      }
    });

    ingressProcess.once('exit', (code, signal) => {
      if (this.session === session && !session.stopping) {
        session.ingressStatus = 'stopped';
        session.tapStatus = 'stopped';
        this.lastMessage = `The temporary Speaker Engine ingress stopped unexpectedly (${signal || `exit ${code}`}).`;
        this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
      }
    });

    try {
      await this.waitForSink(ingressSinkId);
      session.ingressStatus = 'active';
      session.tapStatus = 'active';

      let successfulBranches = 0;
      for (let i = 0; i < validPhysicalSinks.length; i++) {
        const physicalSink = validPhysicalSinks[i];
        const branchSinkId = `speakerflow.session.${sessionId}.branch.${i}`;
        const branchObj = {
          sinkId: physicalSink.id,
          sinkName: physicalSink.name,
          branchSinkId,
          process: null,
          state: 'starting',
          error: null
        };
        session.branches.push(branchObj);

        try {
          const branchProcess = this.startBranch(branchSinkId, tapSourceId, physicalSink.id);
          branchObj.process = branchProcess;

          branchProcess.once('error', (error) => {
            branchObj.state = 'failed';
            branchObj.error = error.message;
            if (this.session === session && !session.stopping) {
              this.reconcileSessionState();
              this.waveController.updateBranches(session.branches).catch(() => {});
            }
          });

          branchProcess.once('exit', (code, signal) => {
            if (this.session === session && !session.stopping && branchObj.state === 'active') {
              branchObj.state = 'disconnected';
              this.lastMessage = `Branch for "${physicalSink.name}" stopped (${signal || `exit ${code}`}).`;
              this.reconcileSessionState();
              this.waveController.updateBranches(session.branches).catch(() => {});
            }
          });

          branchObj.state = 'active';
          successfulBranches++;
        } catch (branchError) {
          branchObj.state = 'failed';
          branchObj.error = branchError.message;
        }
      }

      if (successfulBranches === 0) {
        throw new Error('All selected output branches failed to start.');
      }

      await this.audioBackend.moveSinkInput(stream.id, ingressSinkId);
      await this.verifyStreamSink(stream.id, ingressSinkId);

      // Give PipeWire a moment to register branch sink-inputs
      await this.waitForBranchSinkInputs(session.branches.filter((b) => b.state === 'active'));

      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);

      this.lastMessage = `Audio routed across ${successfulBranches} physical output branch${successfulBranches === 1 ? '' : 'es'}.`;
    } catch (error) {
      await this.stopSession({ restore: true, preserveMessage: true });
      throw error;
    }
  }

  async stopSession(options = {}) {
    const session = this.session;
    if (!session) return;

    session.stopping = true;
    let restoreError = null;

    await this.waveController.destroy().catch(() => {});

    if (options.restore !== false) {
      try {
        const originalSink = await this.audioBackend.findSinkByName(session.originalSinkId);
        if (!originalSink) {
          throw new Error(`The original output, ${session.originalSinkName}, is unavailable.`);
        }
        const stream = await this.findApplicationStream(session.streamId);
        if (stream?.currentSinkId === session.ingressSinkId) {
          await this.audioBackend.moveSinkInput(session.streamId, originalSink.id);
          await this.verifyStreamSink(session.streamId, originalSink.id);
        }
      } catch (error) {
        restoreError = error;
      }
    }

    const branchStopPromises = session.branches.map((branch) => {
      if (branch.process) {
        return stopProcess(branch.process);
      }
      return Promise.resolve();
    });
    await Promise.all(branchStopPromises);

    if (session.ingressProcess) {
      await stopProcess(session.ingressProcess);
    }

    if (this.session === session) {
      this.session = null;
    }

    if (restoreError) {
      this.lastMessage = `Temporary engine objects were removed, but the original output could not be restored: ${restoreError.message}`;
      throw restoreError;
    }

    if (!options.preserveMessage) {
      this.lastMessage = 'The temporary Multi-Speaker Engine session stopped and the original output was restored.';
    }
  }

  async getWaveStatus() {
    return this.waveController.getStatus();
  }

  async setWaveConfig(config) {
    await this.waveController.setConfig(config);
    return this.waveController.getStatus();
  }

  async setMotionConfig(config) {
    await this.waveController.setConfig({ ...config, engineMode: config.engineMode || 'motion' });
    return this.waveController.getStatus();
  }

  async setMasterVolume(volume) {
    await this.waveController.setMasterVolume(volume);
    return this.waveController.getStatus();
  }

  async setMasterMute(muted) {
    await this.waveController.setMasterMute(muted);
    return this.waveController.getStatus();
  }

  async setSpeakerVolume(sinkId, volume) {
    await this.waveController.setSpeakerVolume(sinkId, volume);
    return this.waveController.getStatus();
  }

  async setSpeakerMute(sinkId, muted) {
    await this.waveController.setSpeakerMute(sinkId, muted);
    return this.waveController.getStatus();
  }

  async disconnectBranch(sinkId) {
    const session = this.session;
    if (!session || session.stopping) {
      throw new Error('No active Speaker Engine session.');
    }

    const branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch) {
      throw new Error(`Branch for output ${sinkId} was not found in active session.`);
    }

    if (branch.state !== 'disconnected') {
      branch.state = 'disconnected';
      if (branch.process) {
        await stopProcess(branch.process);
        branch.process = null;
      }
      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);
      this.lastMessage = `Output "${branch.sinkName}" disconnected from session.`;
    }
  }

  async reconnectBranch(sinkId) {
    const session = this.session;
    if (!session || session.stopping) {
      throw new Error('No active Speaker Engine session.');
    }

    const allSinks = await this.audioBackend.listSinks();
    const physicalSink = allSinks.find((s) => s.name === sinkId);
    if (!physicalSink) {
      throw new Error('The physical output is not currently available in PipeWire.');
    }

    let branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch) {
      const branchIndex = session.branches.length;
      branch = {
        sinkId: physicalSink.name,
        sinkName: physicalSink.description || physicalSink.name,
        branchSinkId: `speakerflow.session.${session.sessionId}.branch.${branchIndex}`,
        process: null,
        state: 'starting',
        error: null
      };
      session.branches.push(branch);
    }

    if (branch.process) {
      await stopProcess(branch.process);
      branch.process = null;
    }

    const branchProcess = this.startBranch(branch.branchSinkId, session.tapSourceId, branch.sinkId);
    branch.process = branchProcess;
    branch.state = 'active';
    branch.error = null;

    branchProcess.once('error', (error) => {
      branch.state = 'failed';
      branch.error = error.message;
      if (this.session === session && !session.stopping) {
        this.reconcileSessionState();
        this.waveController.updateBranches(session.branches).catch(() => {});
      }
    });

    branchProcess.once('exit', (code, signal) => {
      if (this.session === session && !session.stopping && branch.state === 'active') {
        branch.state = 'disconnected';
        this.lastMessage = `Branch for "${branch.sinkName}" stopped (${signal || `exit ${code}`}).`;
        this.reconcileSessionState();
        this.waveController.updateBranches(session.branches).catch(() => {});
      }
    });

    // Wait for the reconnected branch sink-input to register in PipeWire
    await this.waitForBranchSinkInputs([branch]);

    this.reconcileSessionState();
    await this.waveController.updateBranches(session.branches);
    this.lastMessage = `Output "${branch.sinkName}" reconnected to session.`;
  }

  async waitForBranchSinkInputs(branchesToWait, timeoutMs = 1500) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      try {
        const sinkInputs = await this.audioBackend.runPactlJson(['--format=json', 'list', 'sink-inputs']);
        const allFound = branchesToWait.every((b) =>
          sinkInputs.some(
            (si) =>
              si.properties?.['node.name'] === `${b.branchSinkId}.playback` ||
              si.properties?.['node.name'] === b.branchSinkId ||
              si.properties?.['device.description'] === b.branchSinkId ||
              si.properties?.['node.group'] === b.branchSinkId
          )
        );
        if (allFound) return;
      } catch {}
      await new Promise((resolve) => setTimeout(resolve, 30));
    }
  }

  startIngressTap(ingressSinkId, tapSourceId) {
    return spawn(
      'pw-loopback',
      [
        '--name',
        ingressSinkId,
        '--group',
        ingressSinkId,
        '--capture-props',
        `{ node.name = "${ingressSinkId}" media.class = "Audio/Sink" node.virtual = true }`,
        '--playback-props',
        `{ node.name = "${tapSourceId}" media.class = "Audio/Source" node.virtual = true }`
      ],
      { stdio: ['ignore', 'ignore', 'pipe'] }
    );
  }

  startBranch(branchSinkId, tapSourceId, physicalSinkId) {
    return spawn(
      'pw-loopback',
      [
        '--name',
        branchSinkId,
        '--group',
        branchSinkId,
        '--capture',
        tapSourceId,
        '--capture-props',
        `{ node.name = "${branchSinkId}.capture" node.passive = true }`,
        '--playback',
        physicalSinkId,
        '--playback-props',
        `{ node.name = "${branchSinkId}.playback" node.passive = true node.dont-reconnect = true }`
      ],
      { stdio: ['ignore', 'ignore', 'pipe'] }
    );
  }

  async reconcileSession() {
    const session = this.session;
    if (!session || session.stopping) return;

    const sinks = await this.audioBackend.listSinks();
    const sinkNames = new Set(sinks.map((s) => s.name));

    let changed = false;
    for (const branch of session.branches) {
      if (branch.state === 'active') {
        if (!sinkNames.has(branch.sinkId)) {
          branch.state = 'disconnected';
          branch.error = 'Physical sink disconnected.';
          if (branch.process) {
            stopProcess(branch.process).catch(() => {});
          }
          this.lastMessage = `Output "${branch.sinkName}" disconnected. Branch disabled.`;
          changed = true;
        }
      }
    }

    if (changed) {
      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);
    }
  }

  reconcileSessionState() {
    const session = this.session;
    if (!session) return;

    const activeCount = session.branches.filter((b) => b.state === 'active').length;
    if (activeCount === 0) {
      session.state = 'degraded';
      this.lastMessage = 'No selected physical outputs are currently available.';
    } else if (activeCount < session.branches.length) {
      session.state = 'degraded';
    } else {
      session.state = 'running';
    }
  }

  async findApplicationStream(streamId) {
    const streams = await this.audioBackend.listApplicationStreams();
    return streams.find((stream) => stream.id === Number(streamId)) || null;
  }

  async verifyStreamSink(streamId, expectedSinkId, timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const stream = await this.findApplicationStream(streamId);
      if (stream && stream.currentSinkId === expectedSinkId) {
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    const stream = await this.findApplicationStream(streamId);
    if (!stream) {
      throw new Error('The application stream disappeared before routing could be verified.');
    }
    throw new Error(`The application stream did not reach the expected temporary audio path (current: ${stream.currentSinkId}, expected: ${expectedSinkId}).`);
  }

  async waitForSink(sinkId) {
    const deadline = Date.now() + 3000;
    while (Date.now() < deadline) {
      const sink = await this.audioBackend.findSinkByName(sinkId);
      if (sink) return;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error('SpeakerFlow could not create its temporary ingress sink.');
  }
}

function stopProcess(child) {
  if (!child || child.exitCode !== null || child.signalCode) return Promise.resolve();

  return new Promise((resolve) => {
    let resolved = false;
    const cleanup = () => {
      if (!resolved) {
        resolved = true;
        resolve();
      }
    };

    const timeout = setTimeout(() => {
      if (child.exitCode === null && !child.signalCode) {
        try {
          child.kill('SIGKILL');
        } catch {}
      }
      cleanup();
    }, 1500);

    child.once('exit', () => {
      clearTimeout(timeout);
      cleanup();
    });

    try {
      child.kill('SIGTERM');
    } catch {
      cleanup();
    }
  });
}

module.exports = { SpeakerEngineService };
