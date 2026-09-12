const { spawn, execFile } = require('node:child_process');
const { promisify } = require('node:util');
const { AudioBackend } = require('./audioBackend.cjs');

const execFileAsync = promisify(execFile);

class LinuxPulseAudioBackend extends AudioBackend {
  constructor() {
    super();
    this._subscribeProcess = null;
    this._monitoringActive = false;
    this._restartTimer = null;
  }

  startMonitoring() {
    this._monitoringActive = true;
    if (this._subscribeProcess) return;
    if (this._restartTimer) {
      clearTimeout(this._restartTimer);
      this._restartTimer = null;
    }

    try {
      this._subscribeProcess = spawn('pactl', ['subscribe']);

      let debounceTimer = null;
      let pendingEvents = { devices: false, streams: false, defaultSink: false };

      const flushEvents = () => {
        if (pendingEvents.devices) {
          this.emit('devices-changed');
        }
        if (pendingEvents.streams) {
          this.emit('streams-changed');
        }
        if (pendingEvents.defaultSink) {
          this.emit('default-sink-changed');
        }
        pendingEvents = { devices: false, streams: false, defaultSink: false };
      };

      let buffer = '';
      this._subscribeProcess.stdout.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';

        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed) continue;

          if (trimmed.includes("on sink #") || trimmed.includes("on card #")) {
            pendingEvents.devices = true;
          } else if (trimmed.includes("on sink-input #")) {
            pendingEvents.streams = true;
          } else if (trimmed.includes("on server #")) {
            pendingEvents.devices = true;
            pendingEvents.defaultSink = true;
          }
        }

        if (pendingEvents.devices || pendingEvents.streams || pendingEvents.defaultSink) {
          if (debounceTimer) clearTimeout(debounceTimer);
          debounceTimer = setTimeout(flushEvents, 60);
        }
      });

      this._subscribeProcess.on('error', () => {
        this._subscribeProcess = null;
        this._scheduleMonitoringRestart();
      });

      this._subscribeProcess.on('exit', () => {
        this._subscribeProcess = null;
        this._scheduleMonitoringRestart();
      });
    } catch {
      this._subscribeProcess = null;
      this._scheduleMonitoringRestart();
    }
  }

  _scheduleMonitoringRestart() {
    if (!this._monitoringActive || this._restartTimer) return;
    this._restartTimer = setTimeout(() => {
      this._restartTimer = null;
      if (this._monitoringActive) {
        this.startMonitoring();
      }
    }, 1000);
  }

  stopMonitoring() {
    this._monitoringActive = false;
    if (this._restartTimer) {
      clearTimeout(this._restartTimer);
      this._restartTimer = null;
    }
    if (this._subscribeProcess) {
      try {
        this._subscribeProcess.kill('SIGTERM');
      } catch {}
      this._subscribeProcess = null;
    }
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
      isVirtual: sink.properties?.['node.virtual'] === 'true',
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
          iconName: properties['application.icon_name'] || properties['application.process.binary'] || 'audio-x-generic'
        };
      });
  }

  async findSinkByName(sinkName) {
    const sinks = await this.listSinks();
    const sink = sinks.find((item) => item.name === sinkName);

    return sink
      ? {
          id: sink.name,
          index: sink.index,
          name: sink.description || sink.name,
          isVirtual: sink.properties?.['node.virtual'] === 'true',
          volumePercent: parseVolumePercent(sink.volume),
          mute: Boolean(sink.mute)
        }
      : null;
  }

  async moveSinkInput(streamId, sinkName) {
    try {
      await execFileAsync('pactl', ['move-sink-input', String(streamId), sinkName]);
    } catch (error) {
      throw new Error(`Unable to move stream ${streamId}: ${readableError(error)}`);
    }
  }

  async setStreamVolume(streamId, volumePercent) {
    try {
      const clamped = Math.max(0, Math.min(150, Math.round(volumePercent)));
      await execFileAsync('pactl', ['set-sink-input-volume', String(streamId), `${clamped}%`]);
    } catch (error) {
      throw new Error(`Unable to set volume for stream ${streamId}: ${readableError(error)}`);
    }
  }

  async setSinkInputVolume(sinkInputId, volumePercent) {
    try {
      const clamped = Math.max(0, Math.min(150, Math.round(volumePercent)));
      await execFileAsync('pactl', ['set-sink-input-volume', String(sinkInputId), `${clamped}%`]);
    } catch (error) {
      throw new Error(`Unable to set volume for sink-input ${sinkInputId}: ${readableError(error)}`);
    }
  }

  async setStreamMute(streamId, muted) {
    try {
      await execFileAsync('pactl', ['set-sink-input-mute', String(streamId), muted ? '1' : '0']);
    } catch (error) {
      throw new Error(`Unable to set mute for stream ${streamId}: ${readableError(error)}`);
    }
  }

  async setSinkVolume(sinkName, volumePercent) {
    try {
      const clamped = Math.max(0, Math.min(150, Math.round(volumePercent)));
      await execFileAsync('pactl', ['set-sink-volume', String(sinkName), `${clamped}%`]);
    } catch (error) {
      throw new Error(`Unable to set volume for sink ${sinkName}: ${readableError(error)}`);
    }
  }

  async setSinkMute(sinkName, muted) {
    try {
      await execFileAsync('pactl', ['set-sink-mute', String(sinkName), muted ? '1' : '0']);
    } catch (error) {
      throw new Error(`Unable to set mute for sink ${sinkName}: ${readableError(error)}`);
    }
  }

  async listSinks() {
    try {
      return await this.runPactlJson(['--format=json', 'list', 'sinks']);
    } catch (error) {
      throw new Error(`Unable to read PipeWire/PulseAudio output devices: ${readableError(error)}`);
    }
  }

  async listSinkInputs() {
    try {
      return await this.runPactlJson(['--format=json', 'list', 'sink-inputs']);
    } catch (error) {
      throw new Error(`Unable to read PipeWire/PulseAudio sink-inputs: ${readableError(error)}`);
    }
  }

  async runPactlJson(args) {
    const { stdout } = await execFileAsync('pactl', args, { maxBuffer: 1024 * 1024 });
    return JSON.parse(stdout);
  }

  async getDefaultOutputId() {
    try {
      const { stdout } = await execFileAsync('pactl', ['get-default-sink']);
      return stdout.trim();
    } catch (error) {
      throw new Error(`Unable to read the default output device: ${readableError(error)}`);
    }
  }

  async setDefaultOutput(sinkId) {
    try {
      await execFileAsync('pactl', ['set-default-sink', String(sinkId)]);
    } catch (error) {
      throw new Error(`Unable to set default output device to ${sinkId}: ${readableError(error)}`);
    }
  }

  async listOutputDevicesWithStatus() {
    const [devices, defaultOutputId] = await Promise.all([
      this.listOutputDevices(),
      this.getDefaultOutputId()
    ]);

    return devices.map((device) => ({
      ...device,
      isDefault: device.id === defaultOutputId
    }));
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
  if (state === 'RUNNING') return 'active';
  if (state === 'SUSPENDED') return 'idle';
  return 'unavailable';
}

function readableError(error) {
  return error.stderr?.trim() || error.message;
}

module.exports = { LinuxPulseAudioBackend };
