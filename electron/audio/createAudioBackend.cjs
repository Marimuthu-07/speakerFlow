const { LinuxPulseAudioBackend } = require('./linuxPulseAudioBackend.cjs');

function createAudioBackend(platform = process.platform) {
  if (platform === 'linux') {
    return new LinuxPulseAudioBackend();
  }

  throw new Error(`SpeakerFlow does not yet support audio discovery on ${platform}.`);
}

module.exports = { createAudioBackend };
