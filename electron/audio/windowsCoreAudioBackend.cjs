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
    this._debounceTimer = null;
    this._pendingEvents = { devices: false, streams: false, defaultSink: false };
  }

  startMonitoring() {
    if (this._monitoringActive) return;
    this._monitoringActive = true;

    if (this._native && typeof this._native.startMonitoring === 'function') {
      try {
        this._native.startMonitoring((event) => this._handleNativeEvent(event));
      } catch (err) {
        console.error('Failed to start Windows Core Audio monitoring:', err);
      }
    }
  }

  _handleNativeEvent(event) {
    if (!this._monitoringActive) return;
    if (!event || !event.type) return;

    if (event.type === 'default-device-changed') {
      this._pendingEvents.defaultSink = true;
    } else if (
      event.type === 'device-added' ||
      event.type === 'device-removed' ||
      event.type === 'device-state-changed'
    ) {
      this._pendingEvents.devices = true;
    } else if (
      event.type === 'session-created' ||
      event.type === 'session-state-changed' ||
      event.type === 'session-disconnected' ||
      event.type === 'session-volume-changed'
    ) {
      this._pendingEvents.streams = true;
    }

    if (this._debounceTimer) clearTimeout(this._debounceTimer);
    this._debounceTimer = setTimeout(() => this._flushEvents(), 60);
  }

  _flushEvents() {
    if (this._pendingEvents.devices) {
      this.emit('devices-changed');
    }
    if (this._pendingEvents.streams) {
      this.emit('streams-changed');
    }
    if (this._pendingEvents.defaultSink) {
      this.emit('default-sink-changed');
    }
    this._pendingEvents = { devices: false, streams: false, defaultSink: false };
  }

  stopMonitoring() {
    this._monitoringActive = false;
    if (this._debounceTimer) {
      clearTimeout(this._debounceTimer);
      this._debounceTimer = null;
    }
    this._pendingEvents = { devices: false, streams: false, defaultSink: false };

    if (this._native && typeof this._native.stopMonitoring === 'function') {
      try {
        this._native.stopMonitoring();
      } catch (err) {
        console.error('Failed to stop Windows Core Audio monitoring:', err);
      }
    }
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

  async setStreamVolume(streamId, volumePercent) {
    if (!this._native || typeof this._native.setSessionVolume !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (streamId === undefined || streamId === null) {
      throw new Error('streamId is required for setStreamVolume');
    }
    const clamped = Math.max(0, Math.min(100, Math.round(Number(volumePercent) || 0)));
    return this._native.setSessionVolume(String(streamId), clamped);
  }

  async setStreamMute(streamId, muted) {
    if (!this._native || typeof this._native.setSessionMute !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (streamId === undefined || streamId === null) {
      throw new Error('streamId is required for setStreamMute');
    }
    return this._native.setSessionMute(String(streamId), Boolean(muted));
  }

  async setSinkVolume(sinkName, volumePercent) {
    if (!this._native || typeof this._native.setEndpointVolume !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (!sinkName) {
      throw new Error('sinkName is required for setSinkVolume');
    }
    const sink = await this.findSinkByName(sinkName);
    const deviceId = sink ? sink.id : String(sinkName);
    const clamped = Math.max(0, Math.min(100, Math.round(Number(volumePercent) || 0)));
    return this._native.setEndpointVolume(deviceId, clamped);
  }

  async setSinkMute(sinkName, muted) {
    if (!this._native || typeof this._native.setEndpointMute !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (!sinkName) {
      throw new Error('sinkName is required for setSinkMute');
    }
    const sink = await this.findSinkByName(sinkName);
    const deviceId = sink ? sink.id : String(sinkName);
    return this._native.setEndpointMute(deviceId, Boolean(muted));
  }

  async setSinkInputVolume(sinkInputId, volumePercent) {
    return this.setStreamVolume(sinkInputId, volumePercent);
  }

  async moveSinkInput(_streamId, _sinkName) {
    throw new Error('Windows Core Audio routing is not implemented yet.');
  }

  async setDefaultOutput(_sinkId) {
    throw new Error('Default audio output control is not implemented yet on Windows.');
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
