const { AudioPipelineAdapter, AudioPipelineSession } = require('./audioPipelineAdapter.cjs');

function cleanupStaleSessionProcesses() {
  // Pure native in-process architecture: zero external background processes spawned by SpeakerFlow.
}

class LinuxPipeWirePipelineSession extends AudioPipelineSession {
  constructor({ sessionId, ingressSinkId, tapSourceId, ingressModuleId, ingressProcess, audioBackend }) {
    super(sessionId, ingressSinkId, tapSourceId);
    this.audioBackend = audioBackend;
    this.ingressModuleId = ingressModuleId || null;
    this.ingressProcess = ingressProcess || null;
    this.branches = new Map(); // branchSinkId -> { handle, moduleId, process, stopping }
    this.destroyed = false;
  }

  async createBranch({ branchSinkId, physicalSinkId, onError, onDisconnect }) {
    if (this.destroyed) {
      throw new Error('Cannot create branch on a destroyed pipeline session.');
    }

    // Stop existing branch with same ID if any
    const existing = this.branches.get(branchSinkId);
    if (existing) {
      existing.stopping = true;
      this.branches.delete(branchSinkId);
      if (existing.moduleId && typeof this.audioBackend?.unloadModule === 'function') {
        try {
          this.audioBackend.unloadModule(existing.moduleId);
        } catch {}
      }
      if (existing.process) {
        await stopProcess(existing.process);
      }
    }

    const branchArgs = `node.name="${branchSinkId}" capture.props={ target.object="${this.tapSourceId}" node.name="${branchSinkId}.capture" node.passive=true } playback.props={ target.object="${physicalSinkId}" node.name="${branchSinkId}.playback" node.passive=true node.dont-reconnect=true }`;

    let branchModuleId = null;
    if (typeof this.audioBackend?.loadModule === 'function') {
      try {
        branchModuleId = this.audioBackend.loadModule('libpipewire-module-loopback', branchArgs);
      } catch (err) {
        if (typeof onError === 'function') onError(err);
        throw new Error(`Failed to create loopback branch for ${branchSinkId}: ${err.message}`);
      }
    } else {
      branchModuleId = 1;
    }

    const handle = { branchSinkId, physicalSinkId };
    const entry = { handle, moduleId: branchModuleId, process: null, stopping: false };
    this.branches.set(branchSinkId, entry);

    try {
      await waitForBranchSinkInput(this.audioBackend, branchSinkId);
      return handle;
    } catch (error) {
      entry.stopping = true;
      this.branches.delete(branchSinkId);
      if (branchModuleId && typeof this.audioBackend?.unloadModule === 'function') {
        try {
          this.audioBackend.unloadModule(branchModuleId);
        } catch {}
      }
      throw error;
    }
  }

  async destroyBranch(branchHandle) {
    const branchSinkId = branchHandle?.branchSinkId || branchHandle;
    if (!branchSinkId) return;

    const entry = this.branches.get(branchSinkId);
    if (entry) {
      entry.stopping = true;
      this.branches.delete(branchSinkId);
      if (entry.moduleId && typeof this.audioBackend?.unloadModule === 'function') {
        try {
          this.audioBackend.unloadModule(entry.moduleId);
        } catch {}
      }
      if (entry.process) {
        await stopProcess(entry.process);
      }
    }
  }

  async destroy() {
    if (this.destroyed) return;
    this.destroyed = true;

    const branchPromises = [];
    for (const entry of this.branches.values()) {
      entry.stopping = true;
      if (entry.moduleId && typeof this.audioBackend?.unloadModule === 'function') {
        try {
          this.audioBackend.unloadModule(entry.moduleId);
        } catch {}
      }
      if (entry.process) {
        branchPromises.push(stopProcess(entry.process));
      }
    }
    this.branches.clear();

    await Promise.all(branchPromises);

    if (this.ingressModuleId && typeof this.audioBackend?.unloadModule === 'function') {
      try {
        this.audioBackend.unloadModule(this.ingressModuleId);
      } catch {}
      this.ingressModuleId = null;
    }

    if (this.ingressProcess) {
      await stopProcess(this.ingressProcess);
      this.ingressProcess = null;
    }
  }
}

class LinuxPipeWirePipelineAdapter extends AudioPipelineAdapter {
  constructor(audioBackend) {
    super();
    this.audioBackend = audioBackend;
  }

  async createPipeline({ sessionId, ingressSinkId, tapSourceId, onIngressError, onIngressExit }) {
    const ingressArgs = `node.name="${ingressSinkId}" node.description="${ingressSinkId}" capture.props={ media.class="Audio/Sink" node.name="${ingressSinkId}" node.virtual=true } playback.props={ media.class="Audio/Source" node.name="${tapSourceId}" node.virtual=true }`;

    let ingressModuleId = null;
    if (typeof this.audioBackend?.loadModule === 'function') {
      try {
        ingressModuleId = this.audioBackend.loadModule('libpipewire-module-loopback', ingressArgs);
      } catch (err) {
        if (typeof onIngressError === 'function') onIngressError(err);
        throw new Error(`Failed to create PipeWire ingress loopback: ${err.message}`);
      }
    } else {
      ingressModuleId = 1;
    }

    try {
      await waitForSink(this.audioBackend, ingressSinkId);
    } catch (error) {
      if (ingressModuleId && typeof this.audioBackend?.unloadModule === 'function') {
        try {
          this.audioBackend.unloadModule(ingressModuleId);
        } catch {}
      }
      throw error;
    }

    return new LinuxPipeWirePipelineSession({
      sessionId,
      ingressSinkId,
      tapSourceId,
      ingressModuleId,
      ingressProcess: null,
      audioBackend: this.audioBackend
    });
  }
}

async function waitForSink(audioBackend, sinkId, isAbortedCheck, timeoutMs = 3000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (typeof isAbortedCheck === 'function') {
      const abortError = isAbortedCheck();
      if (abortError) throw abortError;
    }
    const sink = await audioBackend.findSinkByName(sinkId);
    if (sink) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error('SpeakerFlow could not create its temporary ingress sink.');
}

async function waitForBranchSinkInput(audioBackend, branchSinkId, isAbortedCheck, timeoutMs = 1500) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (typeof isAbortedCheck === 'function') {
      const abortError = isAbortedCheck();
      if (abortError) throw abortError;
    }
    try {
      const sinkInputs = await audioBackend.listSinkInputs();
      const match = sinkInputs.some(
        (si) =>
          si.properties?.['node.name'] === `${branchSinkId}.playback` ||
          si.properties?.['node.name'] === branchSinkId ||
          si.properties?.['device.description'] === branchSinkId ||
          si.properties?.['node.group'] === branchSinkId
      );
      if (match) return;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error(`SpeakerFlow could not create branch "${branchSinkId}".`);
}

function stopProcess(child, gracePeriodMs = 300) {
  if (!child || child.exitCode !== null || child.signalCode) {
    return Promise.resolve();
  }

  return new Promise((resolve) => {
    let resolved = false;
    const cleanup = () => {
      if (!resolved) {
        resolved = true;
        resolve();
      }
    };

    const killTimeout = setTimeout(() => {
      if (child.exitCode === null && !child.signalCode) {
        try {
          child.kill('SIGKILL');
        } catch {}
      }
      setTimeout(cleanup, 50);
    }, gracePeriodMs);

    child.once('exit', () => {
      clearTimeout(killTimeout);
      cleanup();
    });

    try {
      child.kill('SIGTERM');
    } catch {
      clearTimeout(killTimeout);
      cleanup();
    }
  });
}

module.exports = {
  LinuxPipeWirePipelineAdapter,
  LinuxPipeWirePipelineSession,
  cleanupStaleSessionProcesses
};
