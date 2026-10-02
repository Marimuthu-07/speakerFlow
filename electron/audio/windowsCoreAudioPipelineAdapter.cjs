const { AudioPipelineAdapter, AudioPipelineSession } = require('./audioPipelineAdapter.cjs');
const { getNativeBinding } = require('./windowsCoreAudioBackend.cjs');

class WindowsCoreAudioPipelineSession extends AudioPipelineSession {
  constructor({ sessionId, ingressSinkId, tapSourceId, audioBackend, native, targetStream }) {
    super(sessionId, ingressSinkId, tapSourceId);
    this.audioBackend = audioBackend;
    this.native = native;
    this.targetStream = targetStream || null;
    this.branches = new Map(); // branchSinkId -> { handle, physicalSinkId, branchSinkId }
    this.destroyed = false;
  }

  async createBranch({ branchSinkId, physicalSinkId, onError, onDisconnect }) {
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

    try {
      const res = this.native.engineAddOutput({
        branchId: branchSinkId,
        deviceId: physicalSinkId,
        driftCorrectionEnabled: true
      });

      if (!res || !res.success) {
        throw new Error(`Failed to create WASAPI render branch for ${branchSinkId}`);
      }

      const handle = { branchSinkId, physicalSinkId };
      this.branches.set(branchSinkId, handle);
      return handle;
    } catch (err) {
      if (typeof onError === 'function') {
        onError(err);
      }
      throw err;
    }
  }

  async destroyBranch(branchHandle) {
    const branchSinkId = branchHandle?.branchSinkId || branchHandle;
    if (!branchSinkId) return;

    this.branches.delete(branchSinkId);
    try {
      this.native.engineRemoveOutput(branchSinkId);
    } catch {}
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

    // Query available audio endpoints
    const devices = await this.audioBackend.listOutputDevices();

    // Look for an isolated virtual ingress endpoint to eliminate acoustic/digital feedback loops
    // Prioritize standard virtual audio endpoints: Voicemeeter, VB-Audio, Virtual Audio Cable
    const virtualDevice =
      devices.find((d) => d.name === 'Voicemeeter Input (VB-Audio Voicemeeter VAIO)') ||
      devices.find((d) => /voicemeeter input/i.test(d.name)) ||
      devices.find((d) => d.isVirtual) ||
      devices.find((d) => /voicemeeter|vb-audio|virtual|cable/i.test(d.name));

    if (!virtualDevice) {
      const err = new Error(
        'No virtual audio loopback endpoint (such as VB-Audio Voicemeeter or Virtual Audio Cable) was found. ' +
        'An isolated virtual ingress endpoint is required on Windows for feedback-free multi-speaker routing.'
      );
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
      targetStream: stream
    });
  }
}

module.exports = {
  WindowsCoreAudioPipelineAdapter,
  WindowsCoreAudioPipelineSession
};
