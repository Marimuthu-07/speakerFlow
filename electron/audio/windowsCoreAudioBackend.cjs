const path = require('node:path');
const { AudioBackend } = require('./audioBackend.cjs');

let nativeModule = null;

function getNativeBinding() {
  if (nativeModule) return nativeModule;

  const candidatePaths = [
    path.join(__dirname, '../../build/Release/speakerflow_coreaudio.node'),
    path.join(__dirname, '../build/Release/speakerflow_coreaudio.node'),
    path.join(process.cwd(), 'build/Release/speakerflow_coreaudio.node')
  ];

  if (process.resourcesPath) {
    candidatePaths.push(
      path.join(process.resourcesPath, 'app.asar.unpacked/build/Release/speakerflow_coreaudio.node'),
      path.join(process.resourcesPath, 'build/Release/speakerflow_coreaudio.node')
    );
  }

  let lastError = null;
  for (const p of candidatePaths) {
    try {
      nativeModule = require(p);
      return nativeModule;
    } catch (err) {
      lastError = err;
    }
  }

  try {
    nativeModule = require('../../build/Release/speakerflow_coreaudio.node');
    return nativeModule;
  } catch (err) {
    throw new Error(`Failed to load SpeakerFlow Windows Core Audio native addon: ${lastError?.message || err.message}`);
  }
}

class WindowsCoreAudioBackend extends AudioBackend {
  constructor(nativeBinding = null) {
    super();
    this._native = nativeBinding || getNativeBinding();
    if (this._native && typeof this._native.init === 'function') {
      this._native.init();
    }
    this._monitoringActive = false;
  }

  startMonitoring() {
    this._monitoringActive = true;
  }

  stopMonitoring() {
    this._monitoringActive = false;
  }

  destroy() {
    this.stopMonitoring();
    if (this._native && typeof this._native.destroy === 'function') {
      try {
        this._native.destroy();
      } catch {}
    }
  }

  async listOutputDevices() {
    if (!this._native || typeof this._native.listOutputDevices !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }

    const rawDevices = await this._native.listOutputDevices();
    return (rawDevices || []).map((device) => ({
      id: device.id,
      index: 0,
      name: device.name || device.id,
      technicalName: device.id,
      state: 'active',
      isDefault: Boolean(device.isDefault),
      isVirtual: false,
      volumePercent: 100,
      mute: false
    }));
  }

  async getDefaultOutputId() {
    if (!this._native || typeof this._native.getDefaultOutput !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }

    const defaultId = await this._native.getDefaultOutput();
    if (!defaultId) {
      throw new Error('No default audio output device is currently configured in Windows.');
    }
    return defaultId;
  }

  async listOutputDevicesWithStatus() {
    const [devices, defaultOutputId] = await Promise.all([
      this.listOutputDevices(),
      this.getDefaultOutputId().catch(() => null)
    ]);

    return devices.map((device) => ({
      ...device,
      isDefault: defaultOutputId ? device.id === defaultOutputId : device.isDefault
    }));
  }

  async listApplicationStreams() {
    if (!this._native || typeof this._native.listApplicationStreams !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }

    const [devices, rawStreams] = await Promise.all([
      this.listOutputDevices().catch(() => []),
      this._native.listApplicationStreams()
    ]);

    const defaultDevice = devices.find((d) => d.isDefault) || devices[0];

    return (rawStreams || []).map((stream) => ({
      id: stream.id || `${stream.processId}-${stream.currentSinkId}`,
      processId: stream.processId,
      applicationName: stream.name || `Process ${stream.processId}`,
      streamName: stream.name || '',
      currentSinkId: stream.currentSinkId || defaultDevice?.id || null,
      currentSinkName: stream.currentSinkName || defaultDevice?.name || 'Default Output',
      isActive: Boolean(stream.isActive),
      volumePercent: typeof stream.volumePercent === 'number' ? stream.volumePercent : 100,
      mute: Boolean(stream.mute),
      iconName: 'audio-x-generic'
    }));
  }

  async findSinkByName(sinkName) {
    if (!sinkName) return null;
    const devices = await this.listOutputDevices();
    const sink = devices.find(
      (item) => item.id === sinkName || item.name === sinkName || item.technicalName === sinkName
    );

    return sink
      ? {
          id: sink.id,
          index: 0,
          name: sink.name,
          isVirtual: false,
          volumePercent: sink.volumePercent,
          mute: sink.mute
        }
      : null;
  }

  async moveSinkInput(_streamId, _sinkName) {
    throw new Error('Windows Core Audio routing is not implemented yet.');
  }

  async setDefaultOutput(_sinkId) {
    throw new Error('Default audio output control is not implemented yet on Windows.');
  }

  async setStreamVolume(_streamId, _volumePercent) {
    throw new Error('Stream volume control is not implemented yet on Windows.');
  }

  async setStreamMute(_streamId, _muted) {
    throw new Error('Stream mute control is not implemented yet on Windows.');
  }

  async setSinkVolume(_sinkName, _volumePercent) {
    throw new Error('Sink volume control is not implemented yet on Windows.');
  }

  async setSinkMute(_sinkName, _muted) {
    throw new Error('Sink mute control is not implemented yet on Windows.');
  }

  async setSinkInputVolume(_sinkInputId, _volumePercent) {
    throw new Error('Sink-input volume control is not implemented yet on Windows.');
  }

  async listSinkInputs() {
    throw new Error('Sink-input discovery is not implemented on Windows.');
  }

  loadModule(_name, _args) {
    throw new Error('Module loading is not supported on Windows Core Audio.');
  }

  unloadModule(_moduleId) {
    throw new Error('Module unloading is not supported on Windows Core Audio.');
  }
}

module.exports = { WindowsCoreAudioBackend, getNativeBinding };
