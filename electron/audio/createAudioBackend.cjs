const { LinuxPipeWireAudioBackend } = require('./linuxPipeWireAudioBackend.cjs');
const { LinuxPipeWirePipelineAdapter } = require('./linuxPipeWirePipelineAdapter.cjs');
const { WindowsCoreAudioBackend } = require('./windowsCoreAudioBackend.cjs');
const { WindowsCoreAudioPipelineAdapter } = require('./windowsCoreAudioPipelineAdapter.cjs');

function createAudioBackend(platform = process.platform) {
  if (platform === 'linux') {
    return new LinuxPipeWireAudioBackend();
  }
  if (platform === 'win32') {
    return new WindowsCoreAudioBackend();
  }

  throw new Error(`SpeakerFlow does not yet support audio discovery on ${platform}.`);
}

function createAudioPipelineAdapter(audioBackend, platform = process.platform) {
  if (platform === 'linux') {
    return new LinuxPipeWirePipelineAdapter(audioBackend);
  }
  if (platform === 'win32') {
    return new WindowsCoreAudioPipelineAdapter(audioBackend);
  }

  throw new Error(`SpeakerFlow does not yet support audio pipeline on ${platform}.`);
}

module.exports = { createAudioBackend, createAudioPipelineAdapter };
