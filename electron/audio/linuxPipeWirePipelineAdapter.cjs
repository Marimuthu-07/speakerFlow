const { spawn, execSync } = require('node:child_process');
const { AudioPipelineAdapter, AudioPipelineSession } = require('./audioPipelineAdapter.cjs');

const globalActiveProcesses = new Set();

function terminateAllActiveProcesses() {
  for (const child of globalActiveProcesses) {
    try {
      if (child.exitCode === null && !child.signalCode) {
        child.kill('SIGKILL');
      }
    } catch {}
  }
  globalActiveProcesses.clear();
}

process.on('exit', terminateAllActiveProcesses);
process.on('SIGINT', () => {
  terminateAllActiveProcesses();
  process.exit(130);
});
process.on('SIGTERM', () => {
  terminateAllActiveProcesses();
  process.exit(143);
});

function cleanupStaleSessionProcesses() {
  try {
    execSync("pkill -9 -f 'pw-loopback.*speakerflow\\.session\\.' 2>/dev/null || true");
  } catch {}
}

class LinuxPipeWirePipelineSession extends AudioPipelineSession {
  constructor({ sessionId, ingressSinkId, tapSourceId, ingressProcess, audioBackend }) {
    super(sessionId, ingressSinkId, tapSourceId);
    this.audioBackend = audioBackend;
    this.ingressProcess = ingressProcess;
    this.branches = new Map(); // branchSinkId -> { handle, process, stopping }
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
      if (existing.process) {
        await stopProcess(existing.process);
      }
    }

    const branchProcess = spawn(
      'pw-loopback',
      [
        '--name',
        branchSinkId,
        '--group',
        branchSinkId,
        '--capture',
        this.tapSourceId,
        '--capture-props',
        `{ node.name = "${branchSinkId}.capture" node.passive = true }`,
        '--playback',
        physicalSinkId,
        '--playback-props',
        `{ node.name = "${branchSinkId}.playback" node.passive = true node.dont-reconnect = true }`
      ],
      { stdio: ['ignore', 'ignore', 'pipe'] }
    );

    globalActiveProcesses.add(branchProcess);

    const handle = { branchSinkId, physicalSinkId };
    const entry = { handle, process: branchProcess, stopping: false };
    this.branches.set(branchSinkId, entry);

    let startupDone = false;
    let startupError = null;

    branchProcess.once('error', (error) => {
      if (!startupDone) {
        startupError = error;
      } else if (!entry.stopping && !this.destroyed) {
        if (typeof onError === 'function') {
          onError(error);
        }
      }
    });

    branchProcess.once('exit', (code, signal) => {
      globalActiveProcesses.delete(branchProcess);
      if (!startupDone) {
        startupError = new Error(`Branch process exited during startup (${signal || `exit ${code}`}).`);
      } else if (!entry.stopping && !this.destroyed) {
        if (typeof onDisconnect === 'function') {
          onDisconnect(signal || `exit ${code}`);
        }
      }
    });

    try {
      await waitForBranchSinkInput(this.audioBackend, branchSinkId, () => startupError);
      if (startupError) {
        throw startupError;
      }
      startupDone = true;
      return handle;
    } catch (error) {
      entry.stopping = true;
      this.branches.delete(branchSinkId);
      await stopProcess(branchProcess);
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
      if (entry.process) {
        branchPromises.push(stopProcess(entry.process));
      }
    }
    this.branches.clear();

    await Promise.all(branchPromises);

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
    const ingressProcess = spawn(
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

    globalActiveProcesses.add(ingressProcess);

    let startupDone = false;
    let startupError = null;
    let ingressStopping = false;

    ingressProcess.once('error', (error) => {
      if (!startupDone) {
        startupError = error;
      } else if (!ingressStopping) {
        if (typeof onIngressError === 'function') {
          onIngressError(error);
        }
      }
    });

    ingressProcess.once('exit', (code, signal) => {
      globalActiveProcesses.delete(ingressProcess);
      if (!startupDone) {
        startupError = new Error(`Ingress process exited during startup (${signal || `exit ${code}`}).`);
      } else if (!ingressStopping) {
        if (typeof onIngressExit === 'function') {
          onIngressExit(signal || `exit ${code}`);
        }
      }
    });

    try {
      await waitForSink(this.audioBackend, ingressSinkId, () => startupError);
      if (startupError) {
        throw startupError;
      }
      startupDone = true;
    } catch (error) {
      ingressStopping = true;
      await stopProcess(ingressProcess);
      throw error;
    }

    return new LinuxPipeWirePipelineSession({
      sessionId,
      ingressSinkId,
      tapSourceId,
      ingressProcess,
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
    await new Promise((resolve) => setTimeout(resolve, 100));
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
    await new Promise((resolve) => setTimeout(resolve, 30));
  }
  throw new Error(`SpeakerFlow could not create branch "${branchSinkId}".`);
}

function stopProcess(child, gracePeriodMs = 300) {
  if (!child || child.exitCode !== null || child.signalCode) {
    if (child) globalActiveProcesses.delete(child);
    return Promise.resolve();
  }

  return new Promise((resolve) => {
    let resolved = false;
    const cleanup = () => {
      if (!resolved) {
        resolved = true;
        if (child) globalActiveProcesses.delete(child);
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
