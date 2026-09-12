const { LinuxPulseAudioBackend } = require('./linuxPulseAudioBackend.cjs');
const { LinuxPipeWirePipelineAdapter } = require('./linuxPipeWirePipelineAdapter.cjs');

function createAudioBackend(platform = process.platform) {
  if (platform === 'linux') {
    return new LinuxPulseAudioBackend();
  }

  throw new Error(`SpeakerFlow does not yet support audio discovery on ${platform}.`);
}

function createAudioPipelineAdapter(audioBackend, platform = process.platform) {
  if (platform === 'linux') {
    return new LinuxPipeWirePipelineAdapter(audioBackend);
  }

  throw new Error(`SpeakerFlow does not yet support audio pipeline on ${platform}.`);
}

module.exports = { createAudioBackend, createAudioPipelineAdapter };
