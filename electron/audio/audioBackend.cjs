const { EventEmitter } = require('node:events');

class AudioBackend extends EventEmitter {
  async listOutputDevices() {
    throw new Error('Audio device discovery is not implemented for this platform.');
  }

  async listApplicationStreams() {
    throw new Error('Application stream discovery is not implemented for this platform.');
  }

  async findSinkByName() {
    throw new Error('Sink lookup is not implemented for this platform.');
  }

  async moveSinkInput() {
    throw new Error('Audio stream routing is not implemented for this platform.');
  }

  async setStreamVolume() {
    throw new Error('Stream volume control is not implemented for this platform.');
  }

  async setStreamMute() {
    throw new Error('Stream mute control is not implemented for this platform.');
  }

  async setSinkVolume() {
    throw new Error('Sink volume control is not implemented for this platform.');
  }

  async setSinkMute() {
    throw new Error('Sink mute control is not implemented for this platform.');
  }

  async setSinkInputVolume() {
    throw new Error('Sink-input volume control is not implemented for this platform.');
  }

  async listSinkInputs() {
    throw new Error('Sink-input discovery is not implemented for this platform.');
  }

  async getDefaultOutputId() {
    throw new Error('Default audio output discovery is not implemented for this platform.');
  }

  async setDefaultOutput() {
    throw new Error('Default audio output control is not implemented for this platform.');
  }

  startMonitoring() {}

  stopMonitoring() {}
}

module.exports = { AudioBackend };
