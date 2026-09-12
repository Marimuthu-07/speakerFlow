class AudioPipelineAdapter {
  async createPipeline(_options) {
    throw new Error('createPipeline is not implemented for this platform.');
  }
}

class AudioPipelineSession {
  constructor(sessionId, ingressSinkId, tapSourceId) {
    this.sessionId = sessionId;
    this.ingressSinkId = ingressSinkId;
    this.tapSourceId = tapSourceId;
  }

  async createBranch(_options) {
    throw new Error('createBranch is not implemented for this platform.');
  }

  async destroyBranch(_branchHandle) {
    throw new Error('destroyBranch is not implemented for this platform.');
  }

  async destroy() {
    throw new Error('destroy is not implemented for this platform.');
  }
}

module.exports = { AudioPipelineAdapter, AudioPipelineSession };
