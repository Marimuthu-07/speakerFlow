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

function matchesStreamId(item, streamId) {
  if (!item) return false;
  if (
    item.id === streamId ||
    String(item.id) === String(streamId) ||
    (typeof item.id === 'number' && item.id === Number(streamId))
  ) {
    return true;
  }
  if (Array.isArray(item.sessions)) {
    return item.sessions.some(
      (s) =>
        s.id === streamId ||
        String(s.id) === String(streamId) ||
        (typeof s.id === 'number' && s.id === Number(streamId))
    );
  }
  return false;
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
    return (rawDevices || []).map((device) => {
      const name = device.name || device.id;
      return {
        id: device.id,
        index: 0,
        name,
        technicalName: device.id,
        state: 'active',
        isDefault: Boolean(device.isDefault),
        isVirtual: false,
        volumePercent: typeof device.volumePercent === 'number' ? device.volumePercent : 100,
        mute: Boolean(device.mute)
      };
    });
  }

  async listSinks() {
    return this.listOutputDevices();
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

    // Filter out expired streams (AudioSessionStateExpired = 2)
    const validStreams = (rawStreams || []).filter((s) => s.state !== 2);

    // Group valid audio sessions by processId into unified application streams.
    // If processId is absent or 0, group by session ID so isolated streams are not dropped.
    const streamsByProcess = new Map();
    for (const stream of validStreams) {
      const groupKey = (stream.processId && stream.processId !== 0)
        ? `pid:${stream.processId}`
        : `id:${stream.id || Math.random()}`;
      if (!streamsByProcess.has(groupKey)) {
        streamsByProcess.set(groupKey, []);
      }
      streamsByProcess.get(groupKey).push(stream);
    }

    const groupedStreams = [];
    for (const [_key, pidStreams] of streamsByProcess) {
      if (pidStreams.length === 0) continue;

      // Identify all active sessions for this process
      const activeSessions = pidStreams.filter((s) => Boolean(s.isActive));
      const hasActiveSession = activeSessions.length > 0;

      // Check if this process has an explicit persisted output device configured in Windows
      const persistedSinkId = pidStreams.find((s) => s.persistedSinkId)?.persistedSinkId || null;
      const persistedSink = persistedSinkId ? devices.find((d) => d.id === persistedSinkId) : null;

      // Select representative session deterministically:
      // 1. If process has an explicit persisted sink configured in Windows, prefer session on that sink if present,
      //    else fallback to active session or first session.
      // 2. If any session is active, select an active session (prefer active session with non-zero volume or first active).
      // 3. If all sessions are inactive, prefer session on default device if present, else fallback to the last enumerated session.
      let representative = null;
      if (persistedSink) {
        const onPersisted = pidStreams.find((s) => s.currentSinkId === persistedSink.id);
        representative = onPersisted || (hasActiveSession ? activeSessions[0] : pidStreams[0]);
      } else if (hasActiveSession) {
        representative = activeSessions.find((s) => (s.volumePercent || 0) > 0) || activeSessions[0];
      } else {
        const onDefault = pidStreams.find((s) => s.currentSinkId === defaultDevice?.id);
        representative = onDefault || pidStreams[pidStreams.length - 1];
      }

      if (!representative) {
        representative = pidStreams[0];
      }

      const effectiveSinkId = persistedSink ? persistedSink.id : (representative.currentSinkId || defaultDevice?.id || null);
      const effectiveSinkName = persistedSink ? persistedSink.name : (representative.currentSinkName || defaultDevice?.name || 'Default Output');

      groupedStreams.push({
        id: representative.id || `${representative.processId}-${effectiveSinkId}`,
        processId: representative.processId,
        applicationName: representative.name || `Process ${representative.processId}`,
        streamName: representative.name || '',
        currentSinkId: effectiveSinkId,
        currentSinkName: effectiveSinkName,
        persistedSinkId: persistedSinkId,
        isActive: Boolean(hasActiveSession),
        volumePercent: typeof representative.volumePercent === 'number' ? representative.volumePercent : 100,
        mute: Boolean(representative.mute),
        state: representative.state !== undefined ? representative.state : (hasActiveSession ? 1 : 0),
        iconName: 'audio-x-generic',
        sessions: pidStreams.map((s) => ({
          id: s.id,
          sessionIdentifier: s.sessionIdentifier || null,
          processId: s.processId,
          name: s.name,
          state: s.state,
          isActive: Boolean(s.isActive),
          volumePercent: typeof s.volumePercent === 'number' ? s.volumePercent : 100,
          mute: Boolean(s.mute),
          currentSinkId: s.currentSinkId,
          currentSinkName: s.currentSinkName,
          persistedSinkId: s.persistedSinkId || null
        }))
      });
    }

    return groupedStreams;
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

  async getEndpointVolume(sinkName) {
    if (!this._native || typeof this._native.getEndpointVolume !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (!sinkName) {
      throw new Error('sinkName is required for getEndpointVolume');
    }
    const sink = await this.findSinkByName(sinkName);
    const deviceId = sink ? sink.id : String(sinkName);
    return this._native.getEndpointVolume(deviceId);
  }

  async getEndpointMute(sinkName) {
    if (!this._native || typeof this._native.getEndpointMute !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (!sinkName) {
      throw new Error('sinkName is required for getEndpointMute');
    }
    const sink = await this.findSinkByName(sinkName);
    const deviceId = sink ? sink.id : String(sinkName);
    return this._native.getEndpointMute(deviceId);
  }

  async getSinkVolume(sinkName) {
    return this.getEndpointVolume(sinkName);
  }

  async getSinkMute(sinkName) {
    return this.getEndpointMute(sinkName);
  }

  async setSinkInputVolume(sinkInputId, volumePercent) {
    return this.setStreamVolume(sinkInputId, volumePercent);
  }

  async moveSinkInput(streamId, sinkName) {
    if (!this._native || typeof this._native.setApplicationOutput !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (streamId === undefined || streamId === null) {
      throw new Error('streamId is required for moveSinkInput');
    }
    if (!sinkName) {
      throw new Error('sinkName is required for moveSinkInput');
    }

    const devices = await this.listOutputDevices();
    const targetDevice = devices.find(
      (d) => d.id === sinkName || d.name === sinkName || d.technicalName === sinkName
    );
    if (!targetDevice) {
      throw new Error(`Target audio endpoint not found: ${sinkName}`);
    }

    const streams = await this.listApplicationStreams();
    const stream = streams.find(
      (s) =>
        matchesStreamId(s, streamId) ||
        String(s.processId) === String(streamId) ||
        (Array.isArray(s.sessions) && s.sessions.some((sess) => matchesStreamId(sess, streamId)))
    );

    if (!stream && !Number.isInteger(Number(streamId))) {
      throw new Error(`Application session not found: ${streamId}`);
    }

    const target = stream ? stream.processId : streamId;
    return this._native.setApplicationOutput(target, targetDevice.id);
  }

  async getApplicationOutput(streamId) {
    if (!this._native || typeof this._native.getApplicationOutput !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (streamId === undefined || streamId === null) {
      throw new Error('streamId is required for getApplicationOutput');
    }
    const streams = await this.listApplicationStreams();
    const stream = streams.find(
      (s) =>
        matchesStreamId(s, streamId) ||
        String(s.processId) === String(streamId) ||
        (Array.isArray(s.sessions) && s.sessions.some((sess) => matchesStreamId(sess, streamId)))
    );
    const target = stream ? stream.processId : streamId;
    return this._native.getApplicationOutput(target);
  }

  async clearApplicationOutput(streamId) {
    if (!this._native || typeof this._native.clearApplicationOutput !== 'function') {
      throw new Error('Windows Core Audio backend is not initialized.');
    }
    if (streamId === undefined || streamId === null) {
      throw new Error('streamId is required for clearApplicationOutput');
    }
    const streams = await this.listApplicationStreams();
    const stream = streams.find(
      (s) =>
        matchesStreamId(s, streamId) ||
        String(s.processId) === String(streamId) ||
        (Array.isArray(s.sessions) && s.sessions.some((sess) => matchesStreamId(sess, streamId)))
    );
    const target = stream ? stream.processId : streamId;
    return this._native.clearApplicationOutput(target);
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
