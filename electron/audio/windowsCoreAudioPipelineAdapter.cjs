const { AudioPipelineAdapter, AudioPipelineSession } = require('./audioPipelineAdapter.cjs');
const { getNativeBinding } = require('./windowsCoreAudioBackend.cjs');

class WindowsCoreAudioPipelineSession extends AudioPipelineSession {
  constructor({
    sessionId,
    ingressSinkId,
    tapSourceId,
    audioBackend,
    native,
    targetStream,
    captureMode = 'endpoint',
    isNativeProcessLoopback = false,
    targetProcessId = null,
    captureInfo = null
  }) {
    super(sessionId, ingressSinkId, tapSourceId);
    this.audioBackend = audioBackend;
    this.native = native;
    this.targetStream = targetStream || null;
    this.captureMode = captureMode;
    this.isNativeProcessLoopback = Boolean(isNativeProcessLoopback);
    this.targetProcessId = targetProcessId;
    this.captureInfo = captureInfo;
    this.branches = new Map(); // branchSinkId -> { handle, physicalSinkId, branchSinkId }
    this.destroyed = false;
  }

  async switchProcess(targetPid) {
    if (this.destroyed) {
      throw new Error('Cannot switch target process on a destroyed pipeline session.');
    }
    if (!this.isNativeProcessLoopback) {
      throw new Error('switchProcess is only supported on native process loopback sessions.');
    }
    const pid = Number(targetPid);
    if (!Number.isInteger(pid) || pid <= 0 || typeof this.native.engineStartProcessCapture !== 'function') {
      throw new Error(`Invalid process ID (${targetPid}) for process loopback capture.`);
    }

    // 1. Snapshot existing branch configurations before native capture restart resets C++ branches
    const branchSnapshots = Array.from(this.branches.values()).map((b) => ({
      branchSinkId: b.branchSinkId,
      physicalSinkId: b.physicalSinkId,
      delayMs: b.delayMs ?? 0,
      volume: typeof this.getBranchVolume === 'function' ? this.getBranchVolume(b.branchSinkId) : 1.0,
      muted: typeof this.isBranchMuted === 'function' ? this.isBranchMuted(b.branchSinkId) : false
    }));

    // 2. Perform process capture switch
    let refDevId = this.ingressSinkId || '';
    if (refDevId === 'process-loopback') refDevId = '';

    let res = null;
    try {
      res = this.native.engineStartProcessCapture(pid, refDevId);
      if (!res || !res.success) {
        throw new Error(`engineStartProcessCapture returned an unsuccessful result for PID ${pid}`);
      }
    } catch (err) {
      // Native StartProcessCapture resets all branches in C++ on restart attempt.
      // Clear this.branches so Electron never refers to destroyed native branches.
      this.branches.clear();
      this.targetProcessId = null;
      throw new Error(`Failed to switch process capture to PID ${pid}: ${err.message}`);
    }

    // Native capture started for the new PID
    this.targetProcessId = pid;
    this.captureInfo = res;

    // 3. Reconstruct all output branches against the new capture stream
    this.branches.clear();
    let reconstructedCount = 0;
    const branchErrors = [];

    for (const snap of branchSnapshots) {
      try {
        const handle = await this.createBranch({
          branchSinkId: snap.branchSinkId,
          physicalSinkId: snap.physicalSinkId,
          delayMs: snap.delayMs
        });
        if (typeof this.setBranchVolume === 'function') {
          this.setBranchVolume(snap.branchSinkId, snap.volume);
        }
        if (typeof this.setBranchMute === 'function') {
          this.setBranchMute(snap.branchSinkId, snap.muted);
        }
        reconstructedCount++;
      } catch (branchErr) {
        branchErrors.push(`Branch ${snap.branchSinkId} (${snap.physicalSinkId}): ${branchErr.message}`);
      }
    }

    if (branchSnapshots.length > 0 && reconstructedCount === 0) {
      throw new Error(
        `Switched capture to PID ${pid}, but all output branches failed to reinitialize: ${branchErrors.join('; ')}`
      );
    }

    return res;
  }

  async createBranch({ branchSinkId, physicalSinkId, delayMs = 0, onError, onDisconnect }) {
    if (this.destroyed) {
      throw new Error('Cannot create branch on a destroyed pipeline session.');
    }

    // Stop existing branch with same ID if any
    const existing = this.branches.get(branchSinkId);
    if (existing) {
      this.branches.delete(branchSinkId);
      try {
        this.native.engineRemoveOutput(branchSinkId);
      } catch {}
    }

    let validatedDelayMs = 0;
    if (delayMs !== undefined && delayMs !== null) {
      const val = Number(delayMs);
      if (Number.isNaN(val) || !Number.isFinite(val) || val < 0 || val > 500) {
        throw new Error('Delay must be a finite number between 0 and 500 milliseconds.');
      }
      validatedDelayMs = val;
    }

    try {
      const res = this.native.engineAddOutput({
        branchId: branchSinkId,
        deviceId: physicalSinkId,
        driftCorrectionEnabled: true,
        delayMs: validatedDelayMs
      });

      if (!res || !res.success) {
        throw new Error(`Failed to create WASAPI render branch for ${branchSinkId}`);
      }

      const handle = {
        branchSinkId,
        physicalSinkId,
        delayMs: res.configuredDelayMs ?? validatedDelayMs,
        delayFrames: res.effectiveDelayFrames ?? 0
      };
      this.branches.set(branchSinkId, handle);
      return handle;
    } catch (err) {
      if (typeof onError === 'function') {
        onError(err);
      }
      throw err;
    }
  }

  async setBranchDelay(branchSinkId, delayMs) {
    if (this.destroyed) {
      throw new Error('Cannot configure branch delay on a destroyed pipeline session.');
    }
    const val = Number(delayMs);
    if (Number.isNaN(val) || !Number.isFinite(val) || val < 0 || val > 500) {
      throw new Error('Delay must be a finite number between 0 and 500 milliseconds.');
    }
    const handle = this.branches.get(branchSinkId);
    if (!handle) {
      throw new Error(`Branch with ID ${branchSinkId} not found in pipeline session.`);
    }
    if (typeof this.native.engineSetBranchDelay === 'function') {
      this.native.engineSetBranchDelay(branchSinkId, val);
    } else if (typeof this.native.engineSetOutputDelay === 'function') {
      this.native.engineSetOutputDelay(branchSinkId, val);
    }
    handle.delayMs = val;
    if (typeof this.native.engineGetBranchDelay === 'function') {
      try {
        const stats = this.native.engineGetBranchDelay(branchSinkId);
        handle.delayFrames = stats.effectiveDelayFrames;
      } catch {}
    }
    return { success: true, branchSinkId, delayMs: val, delayFrames: handle.delayFrames ?? 0 };
  }

  async destroyBranch(branchHandle) {
    const branchSinkId = branchHandle?.branchSinkId || branchHandle;
    if (!branchSinkId) return;

    this.branches.delete(branchSinkId);
    try {
      this.native.engineRemoveOutput(branchSinkId);
    } catch {}
  }

  setMasterVolume(volume) {
    if (this.destroyed || !this.native || typeof this.native.engineSetMasterVolume !== 'function') {
      return;
    }
    try {
      this.native.engineSetMasterVolume(Number(volume));
    } catch {}
  }

  getMasterVolume() {
    if (this.destroyed || !this.native || typeof this.native.engineGetMasterVolume !== 'function') {
      return 1.0;
    }
    try {
      return Number(this.native.engineGetMasterVolume());
    } catch {
      return 1.0;
    }
  }

  setMasterMute(muted) {
    if (this.destroyed || !this.native || typeof this.native.engineSetMasterMute !== 'function') {
      return;
    }
    try {
      this.native.engineSetMasterMute(Boolean(muted));
    } catch {}
  }

  isMasterMuted() {
    if (this.destroyed || !this.native || typeof this.native.engineGetMasterMute !== 'function') {
      return false;
    }
    try {
      return Boolean(this.native.engineGetMasterMute());
    } catch {
      return false;
    }
  }

  setBranchVolume(branchSinkId, volume) {
    if (this.destroyed || !this.native || typeof this.native.engineSetBranchVolume !== 'function') {
      return;
    }
    try {
      this.native.engineSetBranchVolume(branchSinkId, Number(volume));
    } catch {}
  }

  getBranchVolume(branchSinkId) {
    if (this.destroyed || !this.native || typeof this.native.engineGetBranchVolume !== 'function') {
      return 1.0;
    }
    try {
      return Number(this.native.engineGetBranchVolume(branchSinkId));
    } catch {
      return 1.0;
    }
  }

  setBranchMute(branchSinkId, muted) {
    if (this.destroyed || !this.native || typeof this.native.engineSetBranchMute !== 'function') {
      return;
    }
    try {
      this.native.engineSetBranchMute(branchSinkId, Boolean(muted));
    } catch {}
  }

  isBranchMuted(branchSinkId) {
    if (this.destroyed || !this.native || typeof this.native.engineGetBranchMute !== 'function') {
      return false;
    }
    try {
      return Boolean(this.native.engineGetBranchMute(branchSinkId));
    } catch {
      return false;
    }
  }

  getEngineStatus() {
    if (this.destroyed || !this.native || typeof this.native.engineGetStatus !== 'function') {
      return null;
    }
    try {
      const status = this.native.engineGetStatus();
      if (status) {
        status.captureMode = this.captureMode;
        status.isNativeProcessLoopback = this.isNativeProcessLoopback;
        status.targetProcessId = this.targetProcessId;
      }
      return status;
    } catch {
      return null;
    }
  }

  triggerBranchRecovery(branchSinkId, forceImmediate = false) {
    if (this.destroyed || !this.native || typeof this.native.engineTriggerBranchRecovery !== 'function') {
      return false;
    }
    try {
      return Boolean(this.native.engineTriggerBranchRecovery(branchSinkId, Boolean(forceImmediate)));
    } catch {
      return false;
    }
  }

  async destroy() {
    if (this.destroyed) return;
    this.destroyed = true;

    for (const branchSinkId of this.branches.keys()) {
      try {
        this.native.engineRemoveOutput(branchSinkId);
      } catch {}
    }
    this.branches.clear();

    try {
      this.native.engineStopCapture();
    } catch {}

    try {
      this.native.engineShutdown();
    } catch {}
  }
}

class WindowsCoreAudioPipelineAdapter extends AudioPipelineAdapter {
  constructor(audioBackend) {
    super();
    this.audioBackend = audioBackend;
  }

  async createPipeline(options) {
    const { sessionId, ingressSinkId, tapSourceId, stream, streamId, onIngressError, onIngressExit } = options || {};

    const native = this.audioBackend?._native || getNativeBinding();
    if (!native) {
      throw new Error('Windows Core Audio native binding is not available.');
    }

    // Ensure COM / Native binding is initialized
    if (typeof native.init === 'function') {
      native.init();
    }

    // Determine target process ID for Phase 2G native process-loopback capture.
    // Strictly validate PID: must be a positive integer from stream.processId or an explicit numeric PID streamId.
    let targetPid = null;
    if (stream && typeof stream.processId === 'number' && Number.isInteger(stream.processId) && stream.processId > 0) {
      targetPid = stream.processId;
    } else if (!stream && typeof streamId === 'number' && Number.isInteger(streamId) && streamId > 0) {
      targetPid = streamId;
    } else if (!stream && typeof streamId === 'string' && /^\d+$/.test(streamId.trim())) {
      const parsed = Number(streamId.trim());
      if (Number.isInteger(parsed) && parsed > 0) {
        targetPid = parsed;
      }
    }

    let nativeCaptureAttempted = false;
    let nativeCaptureError = null;

    if (targetPid && typeof native.engineStartProcessCapture === 'function') {
      nativeCaptureAttempted = true;
      let referenceDeviceId = '';
      try {
        referenceDeviceId = (await this.audioBackend.getDefaultOutputId().catch(() => '')) || '';
      } catch {}

      try {
        const captureInfo = native.engineStartProcessCapture(targetPid, referenceDeviceId);
        if (captureInfo && captureInfo.success) {
          return new WindowsCoreAudioPipelineSession({
            sessionId,
            ingressSinkId: referenceDeviceId || 'process-loopback',
            tapSourceId: tapSourceId || referenceDeviceId || 'process-loopback',
            audioBackend: this.audioBackend,
            native,
            targetStream: stream,
            captureMode: 'process',
            isNativeProcessLoopback: true,
            targetProcessId: targetPid,
            captureInfo
          });
        }
        nativeCaptureError = new Error('engineStartProcessCapture returned an unsuccessful result.');
      } catch (err) {
        nativeCaptureError = err;
      }

      // If native process capture failed, clean up partial native resources before attempting virtual cable fallback
      try {
        native.engineStopCapture();
        if (typeof native.engineIsCaptureActive === 'function' && native.engineIsCaptureActive()) {
          throw new Error('WASAPI engine capture remained active after stop request.');
        }
      } catch (cleanupErr) {
        const fatalErr = new Error(
          `Native process capture failed (${nativeCaptureError?.message || 'unknown error'}), ` +
          `and partial native resources could not be cleaned up: ${cleanupErr.message}`
        );
        if (typeof onIngressError === 'function') onIngressError(fatalErr);
        throw fatalErr;
      }

      console.warn(
        `[WindowsCoreAudioPipelineAdapter] Native process capture unavailable or failed for PID ${targetPid} ` +
        `(${nativeCaptureError?.message || 'unknown error'}). Falling back to virtual-cable pipeline.`
      );
    }

    // Fallback: Query available audio endpoints for virtual-cable ingress
    const devices = await this.audioBackend.listOutputDevices();

    // Look for an isolated virtual ingress endpoint to eliminate acoustic/digital feedback loops
    // Prioritize standard virtual audio endpoints: Voicemeeter, VB-Audio, Virtual Audio Cable
    const virtualDevice =
      devices.find((d) => d.name === 'Voicemeeter Input (VB-Audio Voicemeeter VAIO)') ||
      devices.find((d) => /voicemeeter input/i.test(d.name)) ||
      devices.find((d) => d.isVirtual) ||
      devices.find((d) => /voicemeeter|vb-audio|virtual|cable/i.test(d.name));

    if (!virtualDevice) {
      const baseMsg =
        'No virtual audio loopback endpoint (such as VB-Audio Voicemeeter or Virtual Audio Cable) was found. ' +
        'An isolated virtual ingress endpoint is required on Windows for feedback-free multi-speaker routing.';
      const errMsg = nativeCaptureAttempted && nativeCaptureError
        ? `Native process capture failed (${nativeCaptureError.message}), and virtual-cable fallback was unavailable: ${baseMsg}`
        : baseMsg;
      const err = new Error(errMsg);
      if (typeof onIngressError === 'function') onIngressError(err);
      throw err;
    }

    const effectiveIngressId = virtualDevice.id;

    // Start WASAPI loopback capture on the virtual ingress endpoint
    let captureInfo = null;
    try {
      captureInfo = native.engineStartCapture(effectiveIngressId);
    } catch (err) {
      if (typeof onIngressError === 'function') onIngressError(err);
      throw new Error(`Failed to start WASAPI loopback capture on ingress endpoint "${virtualDevice.name}": ${err.message}`);
    }

    if (!captureInfo || !captureInfo.success) {
      const err = new Error(`WASAPI loopback capture start failed on "${virtualDevice.name}".`);
      if (typeof onIngressError === 'function') onIngressError(err);
      throw err;
    }

    return new WindowsCoreAudioPipelineSession({
      sessionId,
      ingressSinkId: effectiveIngressId,
      tapSourceId: tapSourceId || effectiveIngressId,
      audioBackend: this.audioBackend,
      native,
      targetStream: stream,
      captureMode: 'endpoint',
      isNativeProcessLoopback: false,
      targetProcessId: null,
      captureInfo
    });
  }
}

module.exports = {
  WindowsCoreAudioPipelineAdapter,
  WindowsCoreAudioPipelineSession
};
