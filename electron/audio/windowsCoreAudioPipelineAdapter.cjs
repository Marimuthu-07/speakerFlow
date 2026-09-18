const { AudioPipelineAdapter } = require('./audioPipelineAdapter.cjs');

class WindowsCoreAudioPipelineAdapter extends AudioPipelineAdapter {
  constructor(audioBackend) {
    super();
    this.audioBackend = audioBackend;
  }

  async createPipeline(_options) {
    throw new Error('Windows multi-speaker pipeline is not implemented yet.');
  }
}

module.exports = { WindowsCoreAudioPipelineAdapter };
