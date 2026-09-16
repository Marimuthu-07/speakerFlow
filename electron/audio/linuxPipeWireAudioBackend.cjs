const path = require('node:path');
const { AudioBackend } = require('./audioBackend.cjs');

let nativeModule = null;

function getNativeBinding() {
  if (nativeModule) return nativeModule;

  const candidatePaths = [
    path.join(__dirname, '../../build/Release/speakerflow_pipewire.node'),
    path.join(__dirname, '../build/Release/speakerflow_pipewire.node'),
    path.join(process.cwd(), 'build/Release/speakerflow_pipewire.node')
  ];

  if (process.resourcesPath) {
    candidatePaths.push(
      path.join(process.resourcesPath, 'app.asar.unpacked/build/Release/speakerflow_pipewire.node'),
      path.join(process.resourcesPath, 'build/Release/speakerflow_pipewire.node')
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
    nativeModule = require('../../build/Release/speakerflow_pipewire.node');
    return nativeModule;
  } catch (err) {
    throw new Error(`Failed to load SpeakerFlow PipeWire native addon: ${lastError?.message || err.message}`);
  }
}

class LinuxPipeWireAudioBackend extends AudioBackend {
  constructor() {
    super();
    this._native = getNativeBinding();
    this._native.init();
    this._monitoringActive = false;
    this._debounceTimer = null;
    this._pendingEvents = { devices: false, streams: false, defaultSink: false };

    this._native.setEventCallback((eventName) => {
      if (!this._monitoringActive) return;

      if (eventName === 'devices-changed') {
        this._pendingEvents.devices = true;
      } else if (eventName === 'streams-changed') {
        this._pendingEvents.streams = true;
      } else if (eventName === 'default-sink-changed') {
        this._pendingEvents.defaultSink = true;
        this._pendingEvents.devices = true;
      }

      if (this._debounceTimer) clearTimeout(this._debounceTimer);
      this._debounceTimer = setTimeout(() => this._flushEvents(), 60);
    });
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

  startMonitoring() {
    this._monitoringActive = true;
  }

  stopMonitoring() {
    this._monitoringActive = false;
    if (this._debounceTimer) {
      clearTimeout(this._debounceTimer);
      this._debounceTimer = null;
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

  async listSinks() {
    return this._native.listSinks();
  }

  async listSinkInputs() {
    return this._native.listSinkInputs();
  }

  async listOutputDevices() {
    const sinks = await this.listSinks();

    return sinks.map((sink) => ({
      id: sink.name,
      index: sink.index,
      name: sink.description || sink.name,
      technicalName: sink.name,
      state: normalizeState(sink.state),
      isDefault: false,
      isVirtual: sink.properties?.['node.virtual'] === 'true' || sink.isVirtual === true,
      volumePercent: parseVolumePercent(sink.volume),
      mute: Boolean(sink.mute)
    }));
  }

  async listApplicationStreams() {
    const [sinks, sinkInputs] = await Promise.all([
      this.listSinks(),
      this.listSinkInputs()
    ]);
    const sinksByIndex = new Map(sinks.map((sink) => [sink.index, sink]));

    return sinkInputs
      .filter(isApplicationStream)
      .map((stream) => {
        const properties = stream.properties || {};
        const sink = sinksByIndex.get(stream.sink);
        const isCorked = stream.corked === true || properties['pulse.corked'] === 'true';

        return {
          id: stream.index,
          applicationName: properties['application.name'] || properties['application.process.binary'] || properties['media.name'] || properties['node.name'] || `Stream ${stream.index}`,
          streamName: properties['media.name'] || properties['node.name'] || '',
          currentSinkId: sink?.name || null,
          currentSinkName: sink?.description || sink?.name || `Unknown sink (${stream.sink})`,
          isActive: !isCorked,
          volumePercent: parseVolumePercent(stream.volume),
          mute: Boolean(stream.mute),
          iconName: properties['application.icon_name'] || properties['application.icon-name'] || properties['application.process.binary'] || 'audio-x-generic'
        };
      });
  }

  async findSinkByName(sinkName) {
    const sinks = await this.listSinks();
    const sink = sinks.find((item) => item.name === sinkName || String(item.index) === String(sinkName));

    return sink
      ? {
          id: sink.name,
          index: sink.index,
          name: sink.description || sink.name,
          isVirtual: sink.properties?.['node.virtual'] === 'true' || sink.isVirtual === true,
          volumePercent: parseVolumePercent(sink.volume),
          mute: Boolean(sink.mute)
        }
      : null;
  }

  async moveSinkInput(streamId, sinkName) {
    try {
      this._native.moveSinkInput(Number(streamId), String(sinkName));
    } catch (error) {
      throw new Error(`Unable to move stream ${streamId}: ${error.message}`);
    }
  }

  async setStreamVolume(streamId, volumePercent) {
    try {
      const clamped = Math.max(0, Math.min(150, Math.round(volumePercent)));
      this._native.setNodeVolume(Number(streamId), clamped);
    } catch (error) {
      throw new Error(`Unable to set volume for stream ${streamId}: ${error.message}`);
    }
  }

  async setSinkInputVolume(sinkInputId, volumePercent) {
    try {
      const clamped = Math.max(0, Math.min(150, Math.round(volumePercent)));
      this._native.setNodeVolume(Number(sinkInputId), clamped);
    } catch (error) {
      throw new Error(`Unable to set volume for sink-input ${sinkInputId}: ${error.message}`);
    }
  }

  async setStreamMute(streamId, muted) {
    try {
      this._native.setNodeMute(Number(streamId), Boolean(muted));
    } catch (error) {
      throw new Error(`Unable to set mute for stream ${streamId}: ${error.message}`);
    }
  }

  async setSinkVolume(sinkName, volumePercent) {
    try {
      const clamped = Math.max(0, Math.min(150, Math.round(volumePercent)));
      const sink = await this.findSinkByName(sinkName);
      if (sink) {
        this._native.setNodeVolume(sink.index, clamped);
      } else if (!isNaN(Number(sinkName))) {
        this._native.setNodeVolume(Number(sinkName), clamped);
      } else {
        throw new Error(`Sink ${sinkName} not found`);
      }
    } catch (error) {
      throw new Error(`Unable to set volume for sink ${sinkName}: ${error.message}`);
    }
  }

  async setSinkMute(sinkName, muted) {
    try {
      const sink = await this.findSinkByName(sinkName);
      if (sink) {
        this._native.setNodeMute(sink.index, Boolean(muted));
      } else if (!isNaN(Number(sinkName))) {
        this._native.setNodeMute(Number(sinkName), Boolean(muted));
      } else {
        throw new Error(`Sink ${sinkName} not found`);
      }
    } catch (error) {
      throw new Error(`Unable to set mute for sink ${sinkName}: ${error.message}`);
    }
  }

  async getDefaultOutputId() {
    try {
      const def = this._native.getDefaultSink();
      if (!def) {
        throw new Error('No default audio sink is currently configured in PipeWire.');
      }
      return def;
    } catch (error) {
      throw new Error(`Unable to read the default output device: ${error.message}`);
    }
  }

  async setDefaultOutput(sinkId) {
    try {
      this._native.setDefaultSink(String(sinkId));
    } catch (error) {
      throw new Error(`Unable to set default output device to ${sinkId}: ${error.message}`);
    }
  }

  async listOutputDevicesWithStatus() {
    const [devices, defaultOutputId] = await Promise.all([
      this.listOutputDevices(),
      this.getDefaultOutputId().catch(() => null)
    ]);

    return devices.map((device) => ({
      ...device,
      isDefault: device.id === defaultOutputId
    }));
  }

  loadModule(name, args) {
    return this._native.loadModule(name, args);
  }

  unloadModule(moduleId) {
    return this._native.unloadModule(moduleId);
  }
}

function parseVolumePercent(volumeObj) {
  if (!volumeObj || typeof volumeObj !== 'object') return 100;
  const channels = Object.values(volumeObj);
  if (channels.length === 0) return 100;
  let totalPercent = 0;
  let count = 0;
  for (const ch of channels) {
    if (ch && typeof ch.value_percent === 'string') {
      totalPercent += parseInt(ch.value_percent.replace('%', ''), 10) || 0;
      count++;
    } else if (ch && typeof ch.value === 'number') {
      totalPercent += Math.round((ch.value / 65536) * 100);
      count++;
    }
  }
  return count > 0 ? Math.round(totalPercent / count) : 100;
}

function isApplicationStream(stream) {
  const properties = stream.properties || {};

  return Boolean(
    stream.client &&
    (properties['application.name'] || properties['application.process.binary']) &&
    properties['media.class'] === 'Stream/Output/Audio' &&
    !properties['node.group']
  );
}

function normalizeState(state) {
  if (!state) return 'unavailable';
  const upper = state.toUpperCase();
  if (upper === 'RUNNING') return 'active';
  if (upper === 'SUSPENDED' || upper === 'IDLE') return 'idle';
  return 'unavailable';
}

module.exports = { LinuxPipeWireAudioBackend, LinuxPulseAudioBackend: LinuxPipeWireAudioBackend };
