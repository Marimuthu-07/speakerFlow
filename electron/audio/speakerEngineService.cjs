const { randomUUID } = require('node:crypto');
const { createAudioPipelineAdapter } = require('./createAudioBackend.cjs');
const { WaveEngineController } = require('./waveEngineController.cjs');

class SpeakerEngineService {
  constructor(audioBackend, pipelineAdapter) {
    this.audioBackend = audioBackend;
    this.pipelineAdapter = pipelineAdapter || createAudioPipelineAdapter(audioBackend);
    this.session = null;
    this.lastMessage = '';
    this.waveController = new WaveEngineController(audioBackend);
    this.reconnectTimers = new Map(); // sinkId -> timeoutHandle
    this.onMasterVolumeChanged = null;
    this.onSessionChanged = null;
    this._isReconciling = false;
    this._reconcilePending = false;
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
            streamActive: this.session.streamActive !== false,
            isManualStreamSelection: Boolean(this.session.isManualStreamSelection),
            originalSinkId: this.session.originalSinkId,
            originalSinkName: this.session.originalSinkName,
            previousDefaultSinkId: this.session.previousDefaultSinkId,
            defaultOutputEstablished: this.session.defaultOutputEstablished,
            ingressSinkId: this.session.ingressSinkId,
            ingressStatus: this.session.ingressStatus,
            tapSourceId: this.session.tapSourceId,
            tapStatus: this.session.tapStatus,
            branches: this.session.branches.map((b) => ({
              sinkId: b.sinkId,
              sinkName: b.sinkName,
              branchSinkId: b.branchSinkId,
              state: b.state,
              error: b.error || null,
              intentionalDisconnect: b.intentionalDisconnect || false
            })),
            activeBranchCount: this.session.branches.filter((b) => b.state === 'active').length,
            totalBranchCount: this.session.branches.length,
            message: this.lastMessage,
            wave: waveStatus
          }
        : { active: false, state: 'idle', message: this.lastMessage, wave: waveStatus }
    };
  }

  async startSession(streamId, selectedSinkIds, isManual = false) {
    if (this.session) {
      throw new Error('A Speaker Engine session is already active.');
    }

    this.clearAllReconnectTimers();

    const sinkIdList = Array.isArray(selectedSinkIds)
      ? selectedSinkIds
      : [selectedSinkIds].filter(Boolean);

    if (sinkIdList.length === 0) {
      throw new Error('At least one physical output must be selected to start the engine.');
    }

    const [stream, allSinks, previousDefaultSinkId] = await Promise.all([
      this.findApplicationStream(streamId),
      this.audioBackend.listSinks(),
      this.audioBackend.getDefaultOutputId().catch(() => null)
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

    const allPhysicalSinksAtStartup = allSinks.filter(
      (sink) =>
        sink.properties?.['node.virtual'] !== 'true' &&
        sink.name !== 'all_speakers' &&
        !sink.name.startsWith('speakerflow.session.')
    );

    const sessionId = randomUUID();
    const ingressSinkId = `speakerflow.session.${sessionId}.ingress`;
    const tapSourceId = `speakerflow.session.${sessionId}.tap`;

    const session = {
      sessionId,
      pipeline: null,
      ingressSinkId,
      tapSourceId,
      ingressStatus: 'starting',
      tapStatus: 'starting',
      state: 'starting',
      streamId: stream.id,
      streamName: stream.streamName ? `${stream.applicationName} — ${stream.streamName}` : stream.applicationName,
      streamActive: stream.isActive,
      isManualStreamSelection: Boolean(isManual),
      originalSinkId: stream.currentSinkId,
      originalSinkName: stream.currentSinkName,
      previousDefaultSinkId,
      defaultOutputEstablished: false,
      branches: [],
      branchSequence: validPhysicalSinks.length,
      selectedSinkIds: new Set(validPhysicalSinks.map((s) => s.id)),
      initialAvailableSinkIds: new Set(allPhysicalSinksAtStartup.map((s) => s.name)),
      stopping: false
    };
    this.session = session;
    this.lastMessage = '';

    try {
      const pipeline = await this.pipelineAdapter.createPipeline({
        sessionId,
        ingressSinkId,
        tapSourceId,
        onIngressError: (error) => {
          if (this.session === session) {
            session.ingressStatus = 'error';
            session.tapStatus = 'error';
            this.lastMessage = `The temporary Speaker Engine ingress could not start: ${error.message}`;
            this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
          }
        },
        onIngressExit: (reason) => {
          if (this.session === session && !session.stopping) {
            session.ingressStatus = 'stopped';
            session.tapStatus = 'stopped';
            this.lastMessage = `The temporary Speaker Engine ingress stopped unexpectedly (${reason}).`;
            this.stopSession({ restore: true, preserveMessage: true }).catch(() => {});
          }
        }
      });

      session.pipeline = pipeline;
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
          handle: null,
          state: 'starting',
          error: null,
          intentionalDisconnect: false,
          retryCount: 0,
          reconnecting: false
        };
        session.branches.push(branchObj);

        try {
          const branchHandle = await pipeline.createBranch({
            branchSinkId,
            physicalSinkId: physicalSink.id,
            onError: (error) => {
              branchObj.state = 'failed';
              branchObj.error = error.message;
              if (this.session === session && !session.stopping) {
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
              }
            },
            onDisconnect: (reason) => {
              if (this.session === session && !session.stopping && branchObj.state === 'active') {
                branchObj.state = 'disconnected';
                branchObj.handle = null;
                this.lastMessage = `Branch for "${physicalSink.name}" stopped (${reason}).`;
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
                if (!branchObj.intentionalDisconnect) {
                  this.scheduleAutoReconnect(branchObj.sinkId);
                }
              }
            }
          });
          branchObj.handle = branchHandle;
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

      // Establish the temporary ingress sink as the system default output
      await this.audioBackend.setDefaultOutput(ingressSinkId);
      await this.verifyDefaultOutput(ingressSinkId);
      session.defaultOutputEstablished = true;
      session.state = 'running';

      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);

      // Perform final reconciliation pass in case device or stream events occurred during startup
      await this.reconcileSession().catch(() => {});

      this.lastMessage = `Audio routed across ${successfulBranches} physical output branch${successfulBranches === 1 ? '' : 'es'}.`;
    } catch (error) {
      await this.stopSession({ restore: true, preserveMessage: true });
      throw error;
    }
  }

  async stopSession(options = {}) {
    const session = this.session;
    this.clearAllReconnectTimers();

    if (!session) return;

    session.stopping = true;
    let restoreError = null;

    await this.waveController.destroy().catch(() => {});

    if (options.restore !== false) {
      // 1. Restore previous system default output BEFORE destroying temporary ingress sink
      if (session.previousDefaultSinkId && session.defaultOutputEstablished) {
        try {
          await this.audioBackend.setDefaultOutput(session.previousDefaultSinkId);
          await this.verifyDefaultOutput(session.previousDefaultSinkId);
          session.defaultOutputEstablished = false;
        } catch (error) {
          if (!restoreError) restoreError = error;
        }
      }

      // 2. Restore application stream to original output
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
        if (!restoreError) restoreError = error;
      }
    }

    if (session.pipeline) {
      await session.pipeline.destroy().catch(() => {});
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
    const clamped = Math.max(0, Math.min(100, Math.round(volume)));
    await this.waveController.setMasterVolume(clamped);
    if (this.session && this.session.ingressSinkId) {
      await this.audioBackend.setSinkVolume(this.session.ingressSinkId, clamped).catch(() => {});
    }
    return this.waveController.getStatus();
  }

  async setMasterMute(muted) {
    const isMuted = Boolean(muted);
    await this.waveController.setMasterMute(isMuted);
    if (this.session && this.session.ingressSinkId) {
      await this.audioBackend.setSinkMute(this.session.ingressSinkId, isMuted).catch(() => {});
    }
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

  async setSessionStream(streamId, isManual = true) {
    const session = this.session;
    if (!session || session.stopping) {
      throw new Error('No active Speaker Engine session.');
    }

    const stream = await this.findApplicationStream(streamId);
    if (!stream) {
      throw new Error(`The selected application stream ${streamId} was not found.`);
    }

    session.isManualStreamSelection = Boolean(isManual);
    session.streamId = stream.id;
    session.streamName = stream.streamName
      ? `${stream.applicationName} — ${stream.streamName}`
      : stream.applicationName;
    session.streamActive = stream.isActive;

    if (stream.currentSinkId !== session.ingressSinkId) {
      await this.audioBackend.moveSinkInput(stream.id, session.ingressSinkId).catch(() => {});
    }

    if (typeof this.onSessionChanged === 'function') {
      this.onSessionChanged();
    }

    return this.getStatus();
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

    branch.intentionalDisconnect = true;
    this.clearReconnectTimer(sinkId);

    if (branch.state !== 'disconnected') {
      branch.state = 'disconnected';
      if (branch.handle && session.pipeline) {
        await session.pipeline.destroyBranch(branch.handle);
        branch.handle = null;
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

    let branch = session.branches.find((b) => b.sinkId === sinkId);
    if (branch) {
      branch.intentionalDisconnect = false;
      branch.retryCount = 0;
      this.clearReconnectTimer(sinkId);
    }

    await this._performBranchReconnect(sinkId, false);
  }

  scheduleAutoReconnect(sinkId, delayMs = 1500) {
    const session = this.session;
    if (!session || session.stopping) return;

    const branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch || branch.intentionalDisconnect || branch.reconnecting) return;

    this.clearReconnectTimer(sinkId);

    const timer = setTimeout(async () => {
      this.reconnectTimers.delete(sinkId);
      if (this.session !== session || session.stopping || branch.intentionalDisconnect || branch.state === 'active') {
        return;
      }
      await this._performBranchReconnect(sinkId, true);
    }, delayMs);

    this.reconnectTimers.set(sinkId, timer);
  }

  clearReconnectTimer(sinkId) {
    const timer = this.reconnectTimers.get(sinkId);
    if (timer) {
      clearTimeout(timer);
      this.reconnectTimers.delete(sinkId);
    }
  }

  clearAllReconnectTimers() {
    for (const timer of this.reconnectTimers.values()) {
      clearTimeout(timer);
    }
    this.reconnectTimers.clear();
  }

  async _performBranchReconnect(sinkId, isAuto = false) {
    const session = this.session;
    if (!session || session.stopping) return;

    const branch = session.branches.find((b) => b.sinkId === sinkId);
    if (!branch) return;
    if (branch.reconnecting) return;
    branch.reconnecting = true;

    try {
      const allSinks = await this.audioBackend.listSinks();
      const physicalSink = allSinks.find((s) => s.name === sinkId);

      if (!physicalSink || physicalSink.properties?.['node.virtual'] === 'true') {
        branch.reconnecting = false;
        if (!isAuto) {
          throw new Error('The physical output is not currently available in PipeWire.');
        }
        branch.state = 'disconnected';
        branch.error = 'Physical sink is not available.';
        branch.retryCount = (branch.retryCount || 0) + 1;
        const nextDelay = Math.min(5000, Math.round(1500 * Math.pow(1.5, Math.min(branch.retryCount, 3))));
        this.scheduleAutoReconnect(sinkId, nextDelay);
        return;
      }

      if (branch.handle && session.pipeline) {
        await session.pipeline.destroyBranch(branch.handle).catch(() => {});
        branch.handle = null;
      }

      if (session.pipeline) {
        const branchHandle = await session.pipeline.createBranch({
          branchSinkId: branch.branchSinkId,
          physicalSinkId: branch.sinkId,
          onError: (error) => {
            branch.state = 'failed';
            branch.error = error.message;
            if (this.session === session && !session.stopping) {
              this.reconcileSessionState();
              this.waveController.updateBranches(session.branches).catch(() => {});
            }
          },
          onDisconnect: (reason) => {
            if (this.session === session && !session.stopping && branch.state === 'active') {
              branch.state = 'disconnected';
              branch.handle = null;
              this.lastMessage = `Branch for "${branch.sinkName}" stopped (${reason}).`;
              this.reconcileSessionState();
              this.waveController.updateBranches(session.branches).catch(() => {});
              if (!branch.intentionalDisconnect) {
                this.scheduleAutoReconnect(branch.sinkId);
              }
            }
          }
        });

        branch.handle = branchHandle;
        branch.state = 'active';
        branch.error = null;
        branch.retryCount = 0;
        this.clearReconnectTimer(sinkId);
        this.reconcileSessionState();
        await this.waveController.updateBranches(session.branches);
        this.lastMessage = isAuto
          ? `Output "${branch.sinkName}" reconnected automatically.`
          : `Output "${branch.sinkName}" reconnected to session.`;
      }
    } catch (error) {
      branch.reconnecting = false;
      branch.state = 'disconnected';
      branch.error = error.message;
      if (isAuto) {
        branch.retryCount = (branch.retryCount || 0) + 1;
        const nextDelay = Math.min(5000, Math.round(1500 * Math.pow(1.5, Math.min(branch.retryCount, 3))));
        this.scheduleAutoReconnect(sinkId, nextDelay);
      } else {
        throw error;
      }
    } finally {
      branch.reconnecting = false;
    }
  }

  async reconcileSession() {
    const session = this.session;
    if (!session || session.stopping || session.state === 'starting') return;

    if (this._isReconciling) {
      this._reconcilePending = true;
      return;
    }

    this._isReconciling = true;
    try {
      do {
        this._reconcilePending = false;
        await this._performReconciliation();
      } while (this._reconcilePending && this.session && !this.session.stopping && this.session.state !== 'starting');
    } finally {
      this._isReconciling = false;
      this._reconcilePending = false;
    }
  }

  async _performReconciliation() {
    const session = this.session;
    if (!session || session.stopping || session.state === 'starting') return;

    const sinks = await this.audioBackend.listSinks();
    const validPhysicalSinks = sinks.filter(
      (sink) =>
        sink.properties?.['node.virtual'] !== 'true' &&
        sink.name !== 'all_speakers' &&
        !sink.name.startsWith('speakerflow.session.')
    );
    const validPhysicalMap = new Map(validPhysicalSinks.map((s) => [s.name, s]));

    // 1. Observe active session ingress sink volume and mute (OS / pavucontrol / Media key master control)
    const ingressSink = sinks.find((s) => s.name === session.ingressSinkId);
    if (ingressSink) {
      const sinkVol = parseSinkVolumePercent(ingressSink.volume);
      const sinkMuted = Boolean(ingressSink.mute);
      const currentWave = this.waveController.getStatus();

      let masterChanged = false;
      if (currentWave.masterVolume !== sinkVol) {
        await this.waveController.setMasterVolume(sinkVol);
        masterChanged = true;
      }
      if (currentWave.masterMuted !== sinkMuted) {
        await this.waveController.setMasterMute(sinkMuted);
        masterChanged = true;
      }

      if (masterChanged && typeof this.onMasterVolumeChanged === 'function') {
        this.onMasterVolumeChanged({ masterVolume: sinkVol, masterMuted: sinkMuted });
      }
    }

    // 2. Reconcile active application stream for session (Auto vs Manual mode)
    let streamChanged = false;
    try {
      const streams = await this.audioBackend.listApplicationStreams();
      const streamList = streams || [];
      const streamMap = new Map(streamList.map((s) => [s.id, s]));
      const currentStream = session.streamId ? streamMap.get(session.streamId) : null;

      let targetStream = null;

      if (session.isManualStreamSelection) {
        if (currentStream) {
          // Manual mode: keep user's stream locked while it exists
          targetStream = currentStream;
        } else {
          // Manually selected stream disappeared: reset manual mode and fallback
          session.isManualStreamSelection = false;
          targetStream = streamList.find((s) => s.isActive) || streamList[0] || null;
        }
      } else {
        // Auto mode:
        if (currentStream && currentStream.isActive) {
          // Current stream is active -> maintain stability (no jumping)
          targetStream = currentStream;
        } else {
          // Current stream is inactive/paused or missing -> prefer active playing stream
          const activeStream = streamList.find((s) => s.isActive);
          if (activeStream) {
            targetStream = activeStream;
          } else if (currentStream) {
            // If no stream is active, stay on current paused stream
            targetStream = currentStream;
          } else {
            // Fallback to first stream or null
            targetStream = streamList[0] || null;
          }
        }
      }

      if (targetStream) {
        if (session.streamId !== targetStream.id) {
          session.streamId = targetStream.id;
          streamChanged = true;
          // Direct newly targeted stream to engine ingress if not already there
          if (targetStream.currentSinkId !== session.ingressSinkId) {
            await this.audioBackend.moveSinkInput(targetStream.id, session.ingressSinkId).catch(() => {});
          }
        }

        const freshName = targetStream.streamName
          ? `${targetStream.applicationName} — ${targetStream.streamName}`
          : targetStream.applicationName;
        const freshActive = targetStream.isActive;

        if (session.streamName !== freshName || session.streamActive !== freshActive) {
          session.streamName = freshName;
          session.streamActive = freshActive;
          streamChanged = true;
        }
      } else {
        if (session.streamActive !== false) {
          session.streamActive = false;
          streamChanged = true;
        }
      }
    } catch {}

    let branchesChanged = false;

    // 3. Clean up branches for physical sinks that no longer exist in the system
    for (let i = session.branches.length - 1; i >= 0; i--) {
      const branch = session.branches[i];
      const isPresent = validPhysicalMap.has(branch.sinkId);

      if (!isPresent) {
        if (branch.state === 'active') {
          branch.state = 'disconnected';
          branch.error = 'Physical sink disconnected.';
          if (branch.handle && session.pipeline) {
            await session.pipeline.destroyBranch(branch.handle).catch(() => {});
            branch.handle = null;
          }
          this.lastMessage = `Output "${branch.sinkName}" disconnected.`;
          branchesChanged = true;
        }
        this.clearReconnectTimer(branch.sinkId);
        if (!branch.intentionalDisconnect) {
          session.branches.splice(i, 1);
          branchesChanged = true;
        }
      } else if (branch.state === 'disconnected' && !branch.intentionalDisconnect && !branch.reconnecting) {
        this.scheduleAutoReconnect(branch.sinkId, 100);
      }
    }

    // 4. Hot-plug newly connected eligible physical outputs into the running session
    const existingSinkIds = new Set(session.branches.map((b) => b.sinkId));
    for (const physicalSink of validPhysicalSinks) {
      if (existingSinkIds.has(physicalSink.name)) continue;

      // Target selection semantics:
      // Must be explicitly selected by the user OR be a brand new device plugged in after session started
      const isSelected = session.selectedSinkIds && session.selectedSinkIds.has(physicalSink.name);
      const isNewlyConnectedDevice = session.initialAvailableSinkIds && !session.initialAvailableSinkIds.has(physicalSink.name);

      if (!isSelected && !isNewlyConnectedDevice) {
        // Device was available at startup but intentionally excluded by the user -> do not attach
        continue;
      }

      existingSinkIds.add(physicalSink.name); // prevent duplicates within loop
      if (session.selectedSinkIds) {
        session.selectedSinkIds.add(physicalSink.name);
      }

      const branchIdx = typeof session.branchSequence === 'number' ? session.branchSequence++ : session.branches.length;
      const branchSinkId = `speakerflow.session.${session.sessionId}.branch.${branchIdx}`;
      const branchObj = {
        sinkId: physicalSink.name,
        sinkName: physicalSink.description || physicalSink.name,
        branchSinkId,
        handle: null,
        state: 'starting',
        error: null,
        intentionalDisconnect: false,
        retryCount: 0,
        reconnecting: false
      };
      session.branches.push(branchObj);

      if (session.pipeline) {
        try {
          const branchHandle = await session.pipeline.createBranch({
            branchSinkId,
            physicalSinkId: physicalSink.name,
            onError: (error) => {
              branchObj.state = 'failed';
              branchObj.error = error.message;
              if (this.session === session && !session.stopping) {
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
              }
            },
            onDisconnect: (reason) => {
              if (this.session === session && !session.stopping && branchObj.state === 'active') {
                branchObj.state = 'disconnected';
                branchObj.handle = null;
                this.lastMessage = `Branch for "${branchObj.sinkName}" stopped (${reason}).`;
                this.reconcileSessionState();
                this.waveController.updateBranches(session.branches).catch(() => {});
                if (!branchObj.intentionalDisconnect) {
                  this.scheduleAutoReconnect(branchObj.sinkId);
                }
              }
            }
          });
          branchObj.handle = branchHandle;
          branchObj.state = 'active';
          this.lastMessage = `New output "${branchObj.sinkName}" connected and added to engine session.`;
          branchesChanged = true;
        } catch (branchErr) {
          branchObj.state = 'failed';
          branchObj.error = branchErr.message;
          branchesChanged = true;
        }
      }
    }

    if (branchesChanged) {
      this.reconcileSessionState();
      await this.waveController.updateBranches(session.branches);
    }

    if ((branchesChanged || streamChanged) && typeof this.onSessionChanged === 'function') {
      this.onSessionChanged();
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

  async verifyDefaultOutput(expectedSinkId, timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      try {
        const currentDefault = await this.audioBackend.getDefaultOutputId();
        if (currentDefault === expectedSinkId) {
          return;
        }
      } catch {}
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    const currentDefault = await this.audioBackend.getDefaultOutputId().catch(() => 'unknown');
    if (currentDefault !== expectedSinkId) {
      throw new Error(`The system default output did not switch to the expected output (current: ${currentDefault}, expected: ${expectedSinkId}).`);
    }
  }
}

function parseSinkVolumePercent(volumeObj) {
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

module.exports = { SpeakerEngineService };
