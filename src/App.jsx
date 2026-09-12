import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

function App() {
  const [activeTab, setActiveTab] = useState('playback'); // 'playback' | 'outputs' | 'spatial' | 'topology'

  const [devices, setDevices] = useState([]);
  const [selectedDeviceIds, setSelectedDeviceIds] = useState(new Set());
  const [loadingDevices, setLoadingDevices] = useState(true);
  const [deviceError, setDeviceError] = useState('');
  const [lastUpdated, setLastUpdated] = useState(null);

  const [streams, setStreams] = useState([]);
  const [aggregate, setAggregate] = useState(null);
  const [selectedStreamId, setSelectedStreamId] = useState(null);
  const [isManualStreamSelection, setIsManualStreamSelection] = useState(false);
  const [loadingStreams, setLoadingStreams] = useState(true);
  const [streamError, setStreamError] = useState('');
  const [routingAction, setRoutingAction] = useState(false);

  const [engineStatus, setEngineStatus] = useState(null);
  const [selectedEngineSinkIds, setSelectedEngineSinkIds] = useState(new Set());
  const [engineAction, setEngineAction] = useState(false);
  const [engineError, setEngineError] = useState('');

  // Wave / Spatial Audio Engine & Mixer State (Phase 6, 7 & 7.1)
  const [waveStatus, setWaveStatus] = useState({
    enabled: false,
    engineMode: 'pattern', // 'pattern' | 'spatial'
    masterVolume: 100,
    masterMuted: false,
    userSpeakerVolumes: {},
    userSpeakerMuted: {},
    // Phase 6 Wave
    pattern: 'circular',
    speed: 0.5,
    direction: 'forward',
    intensity: 1.0,
    pulseWidth: 2,
    effectivePulseWidth: 2,
    chaseFalloff: 0.5,
    order: [],
    activeCount: 0,
    currentTransition: null,
    // Phase 7 Spatial
    spatialPattern: 'orbit',
    sourceX: 0.0,
    sourceY: 0.0,
    currentSourceX: 0.0,
    currentSourceY: 0.0,
    orbitRadius: 0.7,
    spatialFalloff: 0.5,
    speakerPositions: {},
    currentSpatialState: null,
    branchGains: []
  });
  const [waveAction, setWaveAction] = useState(false);
  const [battery, setBattery] = useState({ available: false, percentage: null, state: 'unknown', onAc: false });

  // Dragging state for 2D room canvas
  const svgCanvasRef = useRef(null);
  const [draggingTarget, setDraggingTarget] = useState(null); // 'source' | sinkId | null

  const refreshDevices = useCallback(async () => {
    setLoadingDevices(true);
    setDeviceError('');
    try {
      const nextDevices = await window.speakerFlow.listOutputDevices();
      const deviceIds = new Set(nextDevices.map((device) => device.id));
      setDevices(nextDevices);
      setSelectedDeviceIds((current) => new Set([...current].filter((id) => deviceIds.has(id))));
      setLastUpdated(new Date());
    } catch (refreshError) {
      setDeviceError(refreshError.message || 'Could not detect audio output devices.');
    } finally {
      setLoadingDevices(false);
    }
  }, []);

  const refreshStreams = useCallback(async () => {
    setLoadingStreams(true);
    setStreamError('');
    try {
      const result = await window.speakerFlow.listApplicationStreams();
      const streamList = result.streams || [];
      const streamMap = new Map(streamList.map((stream) => [stream.id, stream]));

      setStreams(streamList);
      setAggregate(result.aggregate);

      setSelectedStreamId((currentId) => {
        const currentStream = currentId ? streamMap.get(currentId) : null;
        const fallbackStream = streamList[0] || null;

        // In manual mode, preserve user's explicitly selected stream as long as it exists
        if (isManualStreamSelection && currentStream) {
          return currentId;
        }

        // If manual stream disappeared, clear manual mode
        if (isManualStreamSelection && !currentStream) {
          setIsManualStreamSelection(false);
        }

        // In auto mode: if current stream is still active, maintain stability (no jumping)
        if (currentStream && currentStream.isActive) {
          return currentId;
        }

        // In auto mode: if current stream is inactive/absent, prefer an active/playing stream
        const activePlayingStream = streamList.find((s) => s.isActive);
        if (activePlayingStream) {
          return activePlayingStream.id;
        }

        // If no stream is active, keep current stream if present
        if (currentStream) {
          return currentId;
        }

        // Fallback to first stream or null
        return fallbackStream ? fallbackStream.id : null;
      });
    } catch (refreshError) {
      setStreamError(refreshError.message || 'Could not detect application audio streams.');
    } finally {
      setLoadingStreams(false);
    }
  }, [isManualStreamSelection]);

  const refreshEngineStatus = useCallback(async () => {
    try {
      const nextStatus = await window.speakerFlow.getSpeakerEngineStatus();
      setEngineStatus(nextStatus);
      if (nextStatus?.session?.wave) {
        setWaveStatus(nextStatus.session.wave);
      }
      if (nextStatus?.session?.active && nextStatus.session.streamId) {
        if (!isManualStreamSelection) {
          setSelectedStreamId(nextStatus.session.streamId);
        }
      }
      setSelectedEngineSinkIds((current) => {
        const availableIds = new Set(nextStatus.outputs.map((o) => o.id));
        const filtered = new Set([...current].filter((id) => availableIds.has(id)));
        if (filtered.size === 0 && nextStatus.outputs.length > 0) {
          return new Set(nextStatus.outputs.map((o) => o.id));
        }
        return filtered;
      });
    } catch (refreshError) {
      setEngineError(refreshError.message || 'Could not inspect the Multi-Speaker Engine.');
    }
  }, [isManualStreamSelection]);

  const refreshWaveStatus = useCallback(async () => {
    try {
      const status = await window.speakerFlow.getWaveStatus();
      if (status) {
        setWaveStatus(status);
      }
    } catch {}
  }, []);

  // Main 5-second polling loop
  useEffect(() => {
    refreshDevices();
    refreshStreams();
    refreshEngineStatus();
    const refreshInterval = window.setInterval(() => {
      refreshDevices();
      refreshStreams();
      refreshEngineStatus();
    }, 5000);
    return () => window.clearInterval(refreshInterval);
  }, [refreshDevices, refreshStreams, refreshEngineStatus]);

  // Faster UI meter refresh (100ms) ONLY when wave/spatial engine is active
  useEffect(() => {
    if (!waveStatus?.enabled || !engineStatus?.session?.active) return;
    const waveInterval = window.setInterval(() => {
      refreshWaveStatus();
    }, 100);
    return () => window.clearInterval(waveInterval);
  }, [waveStatus?.enabled, engineStatus?.session?.active, refreshWaveStatus]);

  // Battery monitoring, live master volume, and real-time backend event listeners
  useEffect(() => {
    if (window.speakerFlow?.getBatteryStatus) {
      window.speakerFlow.getBatteryStatus().then((status) => {
        if (status) setBattery(status);
      }).catch(() => {});
    }
    const unsubBattery = window.speakerFlow?.onBatteryStatusChanged?.((status) => {
      if (status) setBattery(status);
    });
    const unsubMaster = window.speakerFlow?.onMasterVolumeUpdated?.(({ masterVolume, masterMuted }) => {
      setWaveStatus((prev) => prev ? { ...prev, masterVolume, masterMuted } : prev);
      refreshEngineStatus();
    });
    const unsubDevices = window.speakerFlow?.onDevicesUpdated?.(() => {
      refreshDevices();
    });
    const unsubStreams = window.speakerFlow?.onStreamsUpdated?.(() => {
      refreshStreams();
    });
    const unsubEngineStatus = window.speakerFlow?.onEngineStatusUpdated?.(() => {
      refreshEngineStatus();
    });

    return () => {
      if (typeof unsubBattery === 'function') unsubBattery();
      if (typeof unsubMaster === 'function') unsubMaster();
      if (typeof unsubDevices === 'function') unsubDevices();
      if (typeof unsubStreams === 'function') unsubStreams();
      if (typeof unsubEngineStatus === 'function') unsubEngineStatus();
    };
  }, [refreshDevices, refreshStreams, refreshEngineStatus]);

  const selectedStream = useMemo(
    () => streams.find((stream) => stream.id === selectedStreamId) || null,
    [selectedStreamId, streams]
  );
  const engineActive = Boolean(engineStatus?.session?.active);
  const engineSession = engineStatus?.session || null;

  function handleSelectStream(streamId) {
    setIsManualStreamSelection(true);
    setSelectedStreamId(streamId);
    if (engineActive && window.speakerFlow?.setSessionStream) {
      window.speakerFlow.setSessionStream(streamId, true).catch(() => {});
    }
  }

  function toggleDevice(deviceId) {
    setSelectedDeviceIds((current) => {
      const next = new Set(current);
      if (next.has(deviceId)) next.delete(deviceId);
      else next.add(deviceId);
      return next;
    });
  }

  function connectAll() {
    setSelectedDeviceIds(new Set(devices.map((device) => device.id)));
  }

  function toggleEngineSink(sinkId) {
    if (engineActive) return;
    setSelectedEngineSinkIds((current) => {
      const next = new Set(current);
      if (next.has(sinkId)) next.delete(sinkId);
      else next.add(sinkId);
      return next;
    });
  }

  // --- Phase 7.1 Application Stream Controls ---
  async function handleStreamVolumeChange(streamId, volumePercent) {
    setStreams((prev) =>
      prev.map((s) => (s.id === streamId ? { ...s, volumePercent } : s))
    );
    try {
      await window.speakerFlow.setStreamVolume(streamId, volumePercent);
    } catch (err) {
      setStreamError(err.message || 'Could not change stream volume.');
    }
  }

  async function handleStreamMuteToggle(streamId, currentMute) {
    const nextMute = !currentMute;
    setStreams((prev) =>
      prev.map((s) => (s.id === streamId ? { ...s, mute: nextMute } : s))
    );
    try {
      await window.speakerFlow.setStreamMute(streamId, nextMute);
    } catch (err) {
      setStreamError(err.message || 'Could not toggle stream mute.');
    }
  }

  async function handleStreamOutputSelect(streamId, targetSinkId) {
    setRoutingAction(true);
    setStreamError('');
    try {
      await window.speakerFlow.moveStreamOutput(streamId, targetSinkId);
      await Promise.all([refreshStreams(), refreshEngineStatus()]);
    } catch (err) {
      setStreamError(err.message || 'Could not change stream output destination.');
    } finally {
      setRoutingAction(false);
    }
  }

  // --- Phase 7.1 Master Mixer Controls ---
  async function handleMasterVolumeChange(volume) {
    setWaveStatus((prev) => ({ ...prev, masterVolume: volume }));
    try {
      const updated = await window.speakerFlow.setMasterVolume(volume);
      if (updated) setWaveStatus(updated);
    } catch (err) {
      setEngineError(err.message || 'Could not change master volume.');
    }
  }

  async function handleMasterMuteToggle() {
    const nextMuted = !waveStatus.masterMuted;
    setWaveStatus((prev) => ({ ...prev, masterMuted: nextMuted }));
    try {
      const updated = await window.speakerFlow.setMasterMute(nextMuted);
      if (updated) setWaveStatus(updated);
    } catch (err) {
      setEngineError(err.message || 'Could not toggle mute all.');
    }
  }

  // --- Phase 7.1 Per-Speaker Volume & Mute Controls ---
  async function handleSpeakerVolumeChange(sinkId, volume) {
    setWaveStatus((prev) => ({
      ...prev,
      userSpeakerVolumes: { ...(prev.userSpeakerVolumes || {}), [sinkId]: volume }
    }));
    try {
      if (engineActive) {
        const updated = await window.speakerFlow.setSpeakerVolume(sinkId, volume);
        if (updated) setWaveStatus(updated);
      } else {
        await window.speakerFlow.setSinkVolume(sinkId, volume);
        await refreshDevices();
      }
    } catch (err) {
      setEngineError(err.message || 'Could not adjust speaker volume.');
    }
  }

  async function handleSpeakerMuteToggle(sinkId, currentMuted) {
    const nextMuted = !currentMuted;
    setWaveStatus((prev) => ({
      ...prev,
      userSpeakerMuted: { ...(prev.userSpeakerMuted || {}), [sinkId]: nextMuted }
    }));
    try {
      if (engineActive) {
        const updated = await window.speakerFlow.setSpeakerMute(sinkId, nextMuted);
        if (updated) setWaveStatus(updated);
      } else {
        await window.speakerFlow.setSinkMute(sinkId, nextMuted);
        await refreshDevices();
      }
    } catch (err) {
      setEngineError(err.message || 'Could not toggle speaker mute.');
    }
  }

  async function handleDisconnectBranch(sinkId) {
    try {
      await window.speakerFlow.disconnectBranch(sinkId);
      await refreshEngineStatus();
    } catch (err) {
      setEngineError(err.message || 'Could not disconnect output branch.');
    }
  }

  async function handleReconnectBranch(sinkId) {
    try {
      await window.speakerFlow.reconnectBranch(sinkId);
      await refreshEngineStatus();
    } catch (err) {
      setEngineError(err.message || 'Could not reconnect output branch.');
    }
  }

  // Legacy Stream Routing
  async function routeSelectedStream() {
    if (!selectedStream) return;
    setRoutingAction(true);
    setStreamError('');
    try {
      await window.speakerFlow.routeStreamToAllSpeakers(selectedStream.id);
      await refreshStreams();
    } catch (routeError) {
      setStreamError(routeError.message || 'Could not route the selected stream.');
    } finally {
      setRoutingAction(false);
    }
  }

  async function restoreSelectedStream() {
    if (!selectedStream) return;
    setRoutingAction(true);
    setStreamError('');
    try {
      await window.speakerFlow.restoreStreamOutput(selectedStream.id);
      await refreshStreams();
    } catch (restoreError) {
      setStreamError(restoreError.message || 'Could not restore the selected stream.');
    } finally {
      setRoutingAction(false);
    }
  }

  async function startEngineSession() {
    if (!selectedStream || selectedEngineSinkIds.size === 0) return;
    setEngineAction(true);
    setEngineError('');
    try {
      await window.speakerFlow.startSpeakerEngineSession(
        selectedStream.id,
        Array.from(selectedEngineSinkIds),
        isManualStreamSelection
      );
      await Promise.all([refreshStreams(), refreshEngineStatus()]);
    } catch (startError) {
      setEngineError(startError.message || 'Could not start the Multi-Speaker Engine.');
    } finally {
      setEngineAction(false);
    }
  }

  async function stopEngineSession() {
    setEngineAction(true);
    setEngineError('');
    try {
      await window.speakerFlow.stopSpeakerEngineSession();
      await Promise.all([refreshStreams(), refreshEngineStatus()]);
    } catch (stopError) {
      setEngineError(stopError.message || 'Could not stop the Multi-Speaker Engine.');
    } finally {
      setEngineAction(false);
    }
  }

  // Engine Actions
  async function toggleWaveEnabled() {
    if (waveAction) return;
    setWaveAction(true);
    try {
      const nextEnabled = !waveStatus.enabled;
      const updated = await window.speakerFlow.setWaveConfig({ enabled: nextEnabled });
      setWaveStatus(updated);
    } catch (err) {
      setEngineError(err.message || 'Could not toggle engine.');
    } finally {
      setWaveAction(false);
    }
  }

  async function updateEngineMode(engineMode) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ engineMode });
      setWaveStatus(updated);
    } catch {}
  }

  // --- Phase 8 Advanced Motion Engine Methods ---
  async function updateMotionMode(motionMode) {
    try {
      const updated = await window.speakerFlow.setMotionConfig({ motionMode });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateMotionSpeed(motionSpeed) {
    try {
      const updated = await window.speakerFlow.setMotionConfig({ motionSpeed });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateMotionCurve(motionCurve) {
    try {
      const updated = await window.speakerFlow.setMotionConfig({ motionCurve });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateMotionDwell(motionDwellRatio) {
    try {
      const updated = await window.speakerFlow.setMotionConfig({ motionDwellRatio });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateSpatialPathType(spatialPathType) {
    try {
      const updated = await window.speakerFlow.setMotionConfig({ spatialPathType });
      setWaveStatus(updated);
    } catch {}
  }

  async function moveSpeakerInMotionOrder(index, direction) {
    const currentOrder = waveStatus.motion?.order || waveStatus.order || [];
    const activeOrder = [...currentOrder];
    const targetIndex = index + direction;
    if (targetIndex < 0 || targetIndex >= activeOrder.length) return;
    const temp = activeOrder[index];
    activeOrder[index] = activeOrder[targetIndex];
    activeOrder[targetIndex] = temp;
    try {
      const updated = await window.speakerFlow.setMotionConfig({ motionOrder: activeOrder });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateWavePattern(pattern) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ pattern });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateSpatialPattern(spatialPattern) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ spatialPattern });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateWaveSpeed(speed) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ speed });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateWaveDirection(direction) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ direction });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateWaveIntensity(intensity) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ intensity });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateWavePulseWidth(pulseWidth) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ pulseWidth });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateWaveChaseFalloff(chaseFalloff) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ chaseFalloff });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateSourcePosition(x, y) {
    try {
      const clampedX = Math.max(-1.0, Math.min(1.0, x));
      const clampedY = Math.max(-1.0, Math.min(1.0, y));
      const updated = await window.speakerFlow.setWaveConfig({ sourceX: clampedX, sourceY: clampedY });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateOrbitRadius(orbitRadius) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ orbitRadius });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateSpatialFalloff(spatialFalloff) {
    try {
      const updated = await window.speakerFlow.setWaveConfig({ spatialFalloff });
      setWaveStatus(updated);
    } catch {}
  }

  async function updateSpeakerPosition(sinkId, x, y) {
    try {
      const clampedX = Math.max(-1.0, Math.min(1.0, Math.round(x * 100) / 100));
      const clampedY = Math.max(-1.0, Math.min(1.0, Math.round(y * 100) / 100));
      const nextPositions = {
        ...(waveStatus.speakerPositions || {}),
        [sinkId]: { x: clampedX, y: clampedY }
      };
      const updated = await window.speakerFlow.setWaveConfig({ speakerPositions: nextPositions });
      setWaveStatus(updated);
    } catch {}
  }

  async function resetSpeakerPositions() {
    try {
      const activeIds = (engineSession?.branches || []).filter((b) => b.state === 'active').map((b) => b.sinkId);
      const defaultPositions = {};
      const n = activeIds.length;
      if (n === 1) defaultPositions[activeIds[0]] = { x: 0.0, y: 0.0 };
      else if (n === 2) {
        defaultPositions[activeIds[0]] = { x: -0.8, y: 0.5 };
        defaultPositions[activeIds[1]] = { x: 0.8, y: 0.5 };
      } else if (n === 3) {
        defaultPositions[activeIds[0]] = { x: -0.8, y: 0.6 };
        defaultPositions[activeIds[1]] = { x: 0.8, y: 0.6 };
        defaultPositions[activeIds[2]] = { x: 0.0, y: -0.8 };
      } else if (n >= 4) {
        defaultPositions[activeIds[0]] = { x: -0.8, y: 0.8 };
        defaultPositions[activeIds[1]] = { x: 0.8, y: 0.8 };
        defaultPositions[activeIds[2]] = { x: 0.8, y: -0.8 };
        defaultPositions[activeIds[3]] = { x: -0.8, y: -0.8 };
      }
      const updated = await window.speakerFlow.setWaveConfig({ speakerPositions: defaultPositions });
      setWaveStatus(updated);
    } catch {}
  }

  async function moveSpeakerInOrder(index, direction) {
    const activeOrder = [...(waveStatus.order || [])];
    const targetIndex = index + direction;
    if (targetIndex < 0 || targetIndex >= activeOrder.length) return;
    const temp = activeOrder[index];
    activeOrder[index] = activeOrder[targetIndex];
    activeOrder[targetIndex] = temp;
    try {
      const updated = await window.speakerFlow.setWaveConfig({ order: activeOrder });
      setWaveStatus(updated);
    } catch {}
  }

  // Canvas Coordinate Mapping Helpers
  const toSvgX = (x) => 170 + (x ?? 0) * 125;
  const toSvgY = (y) => 170 - (y ?? 0) * 125; // +y is Front (Top)

  function handleCanvasPointerDown(e) {
    if (!svgCanvasRef.current) return;
    const rect = svgCanvasRef.current.getBoundingClientRect();
    const px = e.clientX - rect.left;
    const py = e.clientY - rect.top;
    const normX = Math.max(-1.0, Math.min(1.0, (px - 170) / 125));
    const normY = Math.max(-1.0, Math.min(1.0, (170 - py) / 125));

    if (waveStatus.spatialPattern === 'static') {
      updateSourcePosition(normX, normY);
      setDraggingTarget('source');
    }
  }

  function handleCanvasPointerMove(e) {
    if (!draggingTarget || !svgCanvasRef.current) return;
    const rect = svgCanvasRef.current.getBoundingClientRect();
    const px = e.clientX - rect.left;
    const py = e.clientY - rect.top;
    const normX = Math.max(-1.0, Math.min(1.0, (px - 170) / 125));
    const normY = Math.max(-1.0, Math.min(1.0, (170 - py) / 125));

    if (draggingTarget === 'source') {
      if (waveStatus.spatialPattern === 'static') {
        updateSourcePosition(normX, normY);
      }
    } else {
      updateSpeakerPosition(draggingTarget, normX, normY);
    }
  }

  function handleCanvasPointerUp() {
    setDraggingTarget(null);
  }

  const activeBranches = engineSession?.branches || [];
  const branchMap = useMemo(() => new Map(activeBranches.map((b) => [b.sinkId, b])), [activeBranches]);

  const displaySourceX = waveStatus.enabled ? (waveStatus.currentSourceX ?? waveStatus.sourceX) : waveStatus.sourceX;
  const displaySourceY = waveStatus.enabled ? (waveStatus.currentSourceY ?? waveStatus.sourceY) : waveStatus.sourceY;

  // Available Output Options for Application Streams dropdown
  const physicalSinks = useMemo(
    () => devices.filter((d) => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers'),
    [devices]
  );

  return (
    <main className="app-shell" onPointerUp={handleCanvasPointerUp}>
      {/* Top Header & Branding */}
      <header className="hero">
        <div>
          <p className="eyebrow">SPEAKERFLOW MIXER · PHASE 7.1</p>
          <h1>SpeakerFlow</h1>
          <p className="subtitle">Pavucontrol-style per-application volume, output routing, and multi-speaker mixer.</p>
        </div>
        <div className="hero-actions">
          {(battery?.host?.available ?? battery?.available) && (
            <div
              className={`battery-badge battery-${(battery?.host?.state || battery?.state) || 'discharging'}`}
              title={`Host Battery: ${(battery?.host?.percentage ?? battery?.percentage)}% (${(battery?.host?.state || battery?.state)})`}
            >
              <span className="battery-icon">
                {(battery?.host?.state || battery?.state) === 'charging'
                  ? '⚡'
                  : (battery?.host?.percentage ?? battery?.percentage) > 20
                  ? '🔋'
                  : '🪫'}
              </span>
              <span className="battery-pct">{(battery?.host?.percentage ?? battery?.percentage)}%</span>
            </div>
          )}
          {!(battery?.host?.available ?? battery?.available) && (battery?.host?.onAc ?? battery?.onAc) && (
            <div className="battery-badge battery-ac" title="Connected to AC Power">
              <span className="battery-icon">🔌</span>
              <span className="battery-pct">AC</span>
            </div>
          )}
          <button
            className="button secondary refresh-hero-btn"
            onClick={() => { refreshDevices(); refreshStreams(); refreshEngineStatus(); }}
            disabled={loadingDevices || loadingStreams}
            title="Refresh PipeWire state"
          >
            {loadingDevices || loadingStreams ? 'Refreshing…' : '↻ Refresh'}
          </button>
        </div>
      </header>

      {/* Multi-Speaker Engine Master Volume & Mute Bar */}
      <section className={`master-bar-card ${engineActive ? 'engine-active' : 'engine-idle'}`} aria-labelledby="master-control-heading">
        <div className="master-bar-main">
          <div className="master-info">
            <div className="master-tag-row">
              <span className="master-tag">SPEAKERFLOW ENGINE</span>
              <span className={`master-status-pill ${engineActive ? 'active' : 'idle'}`}>
                {engineActive ? 'Engine Active' : 'Engine Inactive'}
              </span>
            </div>
            <span className="master-title">Engine Master Output</span>
            <span className="master-sub">
              {engineActive
                ? `Multiplies across all ${engineSession?.activeBranchCount || 0} active SpeakerFlow Engine branches (Software gain; hardware output volume remains independent).`
                : 'Controls active SpeakerFlow Engine branches. Start engine in Output Devices or Movement tabs to activate.'}
            </span>
          </div>

          <div className="master-controls">
            <button
              className={`mute-toggle-btn ${waveStatus.masterMuted ? 'muted' : ''}`}
              onClick={handleMasterMuteToggle}
              disabled={!engineActive}
              title={
                !engineActive
                  ? 'Start the Multi-Speaker Engine to enable Master mute'
                  : waveStatus.masterMuted
                  ? 'Unmute All Engine Branches'
                  : 'Mute All Engine Branches'
              }
            >
              {waveStatus.masterMuted ? '🔇 MUTED' : '🔊 Mute All'}
            </button>

            <div className="master-slider-wrap">
              <input
                type="range"
                className="volume-slider master-slider"
                min="0"
                max="100"
                value={waveStatus.masterVolume ?? 100}
                disabled={!engineActive}
                onChange={(e) => {
                  const val = parseInt(e.target.value, 10);
                  handleMasterVolumeChange(val);
                  if (waveStatus.masterMuted && val > 0) {
                    handleMasterMuteToggle();
                  }
                }}
              />
              <span className={`vol-badge ${waveStatus.masterMuted ? 'muted' : ''} ${!engineActive ? 'disabled' : ''}`}>
                {waveStatus.masterMuted
                  ? `${waveStatus.masterVolume ?? 100}% (Muted)`
                  : !engineActive
                  ? `${waveStatus.masterVolume ?? 100}% (Inactive)`
                  : `${waveStatus.masterVolume ?? 100}%`}
              </span>
            </div>
          </div>
        </div>

        {engineActive ? (
          <div className="master-engine-badge active">
            <span className="engine-pulse-dot" />
            <span>SpeakerFlow Engine Active ({engineSession?.activeBranchCount || 0} physical branches scaled by Master Output)</span>
          </div>
        ) : (
          <div className="master-engine-badge inactive">
            <span className="engine-idle-dot" />
            <span>Multi-Speaker Engine is currently idle. Master Output slider and Mute All activate automatically when an engine session starts.</span>
          </div>
        )}
      </section>

      {/* Pavucontrol-style Tab Navigation Bar */}
      <nav className="nav-tabs" aria-label="Audio Mixer Sections">
        <button
          className={`nav-tab-btn ${activeTab === 'playback' ? 'active' : ''}`}
          onClick={() => setActiveTab('playback')}
        >
          <span className="tab-icon">🎵</span>
          <span>Playback</span>
          <span className="tab-count-pill">{streams.length}</span>
        </button>

        <button
          className={`nav-tab-btn ${activeTab === 'outputs' ? 'active' : ''}`}
          onClick={() => setActiveTab('outputs')}
        >
          <span className="tab-icon">🔊</span>
          <span>Output Devices</span>
          <span className="tab-count-pill">{physicalSinks.length}</span>
        </button>

        <button
          className={`nav-tab-btn ${activeTab === 'spatial' ? 'active' : ''}`}
          onClick={() => setActiveTab('spatial')}
        >
          <span className="tab-icon">🧭</span>
          <span>Spatial & Wave Engine</span>
          {waveStatus.enabled && <span className="tab-active-dot" />}
        </button>

        <button
          className={`nav-tab-btn ${activeTab === 'topology' ? 'active' : ''}`}
          onClick={() => setActiveTab('topology')}
        >
          <span className="tab-icon">⚡</span>
          <span>Engine Graph</span>
          {engineActive && <span className="tab-active-dot" />}
        </button>
      </nav>

      {/* Global Error Banner */}
      {(deviceError || streamError || engineError) && (
        <div className="error-toast" role="alert">
          {deviceError || streamError || engineError}
        </div>
      )}

      {/* ========================================================================= */}
      {/* TAB 1: PLAYBACK (Application Streams)                                     */}
      {/* ========================================================================= */}
      {activeTab === 'playback' && (
        <section className="panel tab-panel" aria-labelledby="playback-tab-heading">
          <div className="panel-heading">
            <div>
              <h2 id="playback-tab-heading">Application Playback Streams</h2>
              <p>Control individual application volume, mute, and live output destination.</p>
            </div>
            <span className="count">{streams.length} detected</span>
          </div>

          {streams.length === 0 && !loadingStreams && (
            <div className="empty-card">
              <span className="empty-icon">🎵</span>
              <p>No audio playback streams currently reported by PipeWire.</p>
              <span className="empty-hint">Start playing audio in Strawberry, a browser, or a media player.</span>
            </div>
          )}

          <div className="pavu-list" aria-busy={loadingStreams}>
            {streams.map((stream) => {
              const isSpeakerFlowSession = engineActive && engineSession?.streamId === stream.id;
              const isSelectedStream = selectedStreamId === stream.id;
              const isAllSpeakers = stream.currentSinkId === 'all_speakers';

              return (
                <div
                  className={`pavu-card ${isSpeakerFlowSession ? 'card-speakerflow' : ''} ${isSelectedStream ? 'card-selected-stream' : ''}`}
                  key={stream.id}
                  onClick={() => handleSelectStream(stream.id)}
                  title="Click to select this application stream for Engine Graph"
                >
                  {/* Stream Card Header */}
                  <div className="pavu-card-header">
                    <div className="pavu-app-identity">
                      <span className="pavu-app-icon">
                        {stream.applicationName.toLowerCase().includes('strawberry') ? '🍓' :
                         stream.applicationName.toLowerCase().includes('chrome') || stream.applicationName.toLowerCase().includes('firefox') ? '🌐' :
                         stream.applicationName.toLowerCase().includes('spotify') ? '🎧' : '🎵'}
                      </span>
                      <div className="pavu-app-titles">
                        <div className="pavu-app-name-row">
                          <strong className="pavu-app-name">{stream.applicationName}</strong>
                          <span className={`status-pill ${stream.isActive ? 'playing' : 'paused'}`}>
                            {stream.isActive ? 'playing' : 'paused'}
                          </span>
                          {isSpeakerFlowSession && <span className="badge badge-engine">SpeakerFlow Active</span>}
                          {stream.routedBySpeakerFlow && !isSpeakerFlowSession && <span className="badge">Routed</span>}
                        </div>
                        {stream.streamName && (
                          <div className="pavu-stream-title" title={stream.streamName}>
                            {stream.streamName}
                          </div>
                        )}
                      </div>
                    </div>

                    {/* Output Destination Selector */}
                    <div className="pavu-output-picker">
                      <label className="picker-label">on:</label>
                      <select
                        className="pavu-select"
                        value={
                          isSpeakerFlowSession
                            ? 'speakerflow_engine'
                            : stream.currentSinkId || ''
                        }
                        onChange={(e) => handleStreamOutputSelect(stream.id, e.target.value)}
                        disabled={routingAction || engineAction}
                      >
                        <option value="speakerflow_engine">
                          ✨ SpeakerFlow Multi-Speaker Engine {engineActive ? '(Active)' : ''}
                        </option>
                        {aggregate?.available && (
                          <option value="all_speakers">🌐 All Speakers (Combine Sink)</option>
                        )}
                        <optgroup label="Physical Output Devices">
                          {physicalSinks.map((ps) => (
                            <option value={ps.id} key={ps.id}>
                              {ps.name} {ps.isDefault ? '(Default)' : ''}
                            </option>
                          ))}
                        </optgroup>
                      </select>
                    </div>
                  </div>

                  {/* Volume Slider & Mute Row */}
                  <div className="pavu-volume-row">
                    <button
                      className={`mute-btn ${stream.mute ? 'muted' : ''}`}
                      onClick={() => handleStreamMuteToggle(stream.id, stream.mute)}
                      title={stream.mute ? 'Unmute Stream' : 'Mute Stream'}
                    >
                      {stream.mute ? '🔇' : '🔊'}
                    </button>

                    <div className="pavu-slider-container">
                      <input
                        type="range"
                        className="volume-slider"
                        min="0"
                        max="100"
                        value={stream.mute ? 0 : (stream.volumePercent ?? 100)}
                        onChange={(e) => handleStreamVolumeChange(stream.id, parseInt(e.target.value, 10))}
                      />
                      <span className={`vol-badge ${stream.mute ? 'muted' : ''}`}>
                        {stream.mute ? '0% (Muted)' : `${stream.volumePercent ?? 100}%`}
                      </span>
                    </div>
                  </div>
                </div>
              );
            })}
          </div>
        </section>
      )}

      {/* ========================================================================= */}
      {/* TAB 2: OUTPUT DEVICES (Physical Speakers & SpeakerFlow Branches)          */}
      {/* ========================================================================= */}
      {activeTab === 'outputs' && (
        <section className="panel tab-panel" aria-labelledby="outputs-tab-heading">
          <div className="panel-heading">
            <div>
              <h2 id="outputs-tab-heading">Output Devices & SpeakerFlow Branches</h2>
              <p>
                {engineActive
                  ? 'SpeakerFlow engine is active. Per-speaker sliders control temporary branch gain without altering hardware sink volume.'
                  : 'Adjust physical output hardware volume or configure SpeakerFlow target outputs.'}
              </p>
            </div>
            <span className="count">{physicalSinks.length} detected</span>
          </div>

          <div className="pavu-list">
            {physicalSinks.map((sink) => {
              const branch = branchMap.get(sink.id);
              const isBranchActive = branch && branch.state === 'active';
              const isBranchDisconnected = branch && branch.state === 'disconnected';

              const userVol = waveStatus.userSpeakerVolumes?.[sink.id] ?? 100;
              const spkMuted = waveStatus.userSpeakerMuted?.[sink.id] ?? false;

              // Find telemetry branch gain
              const bg = (waveStatus.branchGains || []).find((g) => g.sinkId === sink.id);
              const dynamicGain = bg?.dynamicGain ?? 100;
              const effectiveGain = bg?.gainPercent ?? Math.round(
                (waveStatus.masterMuted ? 0 : (waveStatus.masterVolume ?? 100) / 100) *
                (spkMuted ? 0 : userVol / 100) *
                (dynamicGain / 100) * 100
              );

              // Find connected Bluetooth battery if available
              const devBattery = (battery?.devices || []).find((d) => {
                if (!d || d.percentage === null || d.percentage === undefined) return false;
                const sinkStr = `${sink.id} ${sink.name} ${sink.technicalName || ''}`.toLowerCase().replace(/_/g, ':');
                const devId = (d.id || '').toLowerCase().replace(/_/g, ':');
                const devName = (d.name || '').toLowerCase();
                return (devId && sinkStr.includes(devId)) || (devName && sinkStr.includes(devName));
              });

              return (
                <div
                  className={`pavu-card ${isBranchActive ? 'card-speakerflow' : isBranchDisconnected ? 'card-disconnected' : ''}`}
                  key={sink.id}
                >
                  {/* Speaker Header */}
                  <div className="pavu-card-header">
                    <div className="pavu-app-identity">
                      <span className="pavu-app-icon">
                        {sink.name.toLowerCase().includes('headphone') ? '🎧' :
                         sink.id.toLowerCase().includes('bluez') ? '📶' : '🔊'}
                      </span>
                      <div className="pavu-app-titles">
                        <div className="pavu-app-name-row">
                          <strong className="pavu-app-name">{sink.name}</strong>
                          {devBattery && (
                            <span className="battery-badge battery-device" title={`Device Battery: ${devBattery.percentage}% (${devBattery.name})`}>
                              <span className="battery-icon">{devBattery.percentage > 20 ? '🔋' : '🪫'}</span>
                              <span className="battery-pct">{devBattery.percentage}%</span>
                            </span>
                          )}
                          <span className={`status-pill ${sink.state}`}>
                            {sink.state}
                          </span>
                          {sink.isDefault && <span className="badge">Default</span>}
                          {isBranchActive && <span className="badge badge-engine">Branch Active</span>}
                          {isBranchDisconnected && <span className="badge badge-warning">Branch Disconnected</span>}
                        </div>
                        <div className="pavu-device-id">{sink.technicalName}</div>
                      </div>
                    </div>

                    {/* Disconnect / Connect Button (SpeakerFlow Session) */}
                    {engineActive && (
                      <div className="pavu-card-actions">
                        {isBranchActive ? (
                          <button
                            className="button secondary btn-disconnect"
                            onClick={() => handleDisconnectBranch(sink.id)}
                            title="Disconnect this branch from the SpeakerFlow engine without interrupting other branches"
                          >
                            Disconnect
                          </button>
                        ) : (
                          <button
                            className="button primary btn-reconnect"
                            onClick={() => handleReconnectBranch(sink.id)}
                            title="Reconnect this speaker to the active SpeakerFlow session"
                          >
                            Connect to Engine
                          </button>
                        )}
                      </div>
                    )}
                  </div>

                  {/* SpeakerFlow Branch Gain Controls (when in Session) */}
                  {engineActive && branch ? (
                    <div className="branch-control-block">
                      <div className="branch-control-tag-row">
                        <span className="branch-tag">SPEAKERFLOW BRANCH GAIN</span>
                        <span className="gain-formula-pill">
                          Target: <strong>{effectiveGain}%</strong> (Master {waveStatus.masterMuted ? '0%' : `${waveStatus.masterVolume ?? 100}%`} × User {spkMuted ? '0%' : `${userVol}%`} × Engine {dynamicGain}%)
                        </span>
                      </div>
                      <p className="branch-help-text">Software gain; hardware output volume remains independent.</p>

                      <div className="pavu-volume-row">
                        <button
                          className={`mute-btn ${spkMuted ? 'muted' : ''}`}
                          onClick={() => handleSpeakerMuteToggle(sink.id, spkMuted)}
                          title={spkMuted ? 'Unmute Speaker Branch' : 'Mute Speaker Branch'}
                        >
                          {spkMuted ? '🔇' : '🔊'}
                        </button>

                        <div className="pavu-slider-container">
                          <input
                            type="range"
                            className="volume-slider branch-slider"
                            min="0"
                            max="100"
                            value={spkMuted ? 0 : userVol}
                            onChange={(e) => handleSpeakerVolumeChange(sink.id, parseInt(e.target.value, 10))}
                            disabled={!isBranchActive}
                          />
                          <span className={`vol-badge ${spkMuted ? 'muted' : ''}`}>
                            {spkMuted ? '0% (Muted)' : `${userVol}%`}
                          </span>
                        </div>
                      </div>
                    </div>
                  ) : (
                    /* Hardware Sink Volume Controls (when not in Session) */
                    <div className="hardware-control-block">
                      <div className="branch-control-tag-row">
                        <span className="hardware-tag">HARDWARE SINK VOLUME</span>
                        <span className="gain-formula-pill">Direct Physical Sink</span>
                      </div>

                      <div className="pavu-volume-row">
                        <button
                          className={`mute-btn ${sink.mute ? 'muted' : ''}`}
                          onClick={() => handleSpeakerMuteToggle(sink.id, sink.mute)}
                          title={sink.mute ? 'Unmute Hardware Sink' : 'Mute Hardware Sink'}
                        >
                          {sink.mute ? '🔇' : '🔊'}
                        </button>

                        <div className="pavu-slider-container">
                          <input
                            type="range"
                            className="volume-slider"
                            min="0"
                            max="100"
                            value={sink.mute ? 0 : (sink.volumePercent ?? 100)}
                            onChange={(e) => handleSpeakerVolumeChange(sink.id, parseInt(e.target.value, 10))}
                          />
                          <span className={`vol-badge ${sink.mute ? 'muted' : ''}`}>
                            {sink.mute ? '0% (Muted)' : `${sink.volumePercent ?? 100}%`}
                          </span>
                        </div>
                      </div>
                    </div>
                  )}
                </div>
              );
            })}
          </div>
        </section>
      )}

      {/* ========================================================================= */}
      {/* TAB 3: SPATIAL & WAVE AUDIO ENGINE (Phase 6 & 7)                          */}
      {/* ========================================================================= */}
      {activeTab === 'spatial' && (
        <section className="panel wave-panel" aria-labelledby="wave-engine-heading">
          <div className="panel-heading">
            <div>
              <h2 id="wave-engine-heading">Spatial & Wave Engine</h2>
              <p>2D room sound-source positioning and rotating wave patterns via independent branch gain modulation.</p>
            </div>
            <span className={`status ${waveStatus.enabled ? 'running' : 'idle'}`}>
              Engine: {waveStatus.enabled ? 'ON' : 'OFF'}
            </span>
          </div>

          <div className="wave-body">
            {/* Movement Mode Switcher Tabs */}
            <div className="engine-mode-tabs">
              <button
                className={`mode-tab-btn ${waveStatus.engineMode === 'motion' ? 'active' : ''}`}
                onClick={() => updateEngineMode('motion')}
              >
                <span className="tab-icon">🚀</span>
                <div className="tab-copy">
                  <strong>Motion Engine (Phase 8)</strong>
                  <span>Sequential, Smooth crossfade, Bounce, Shuffle, and 2D Spatial Paths</span>
                </div>
              </button>
              <button
                className={`mode-tab-btn ${waveStatus.engineMode === 'spatial' ? 'active' : ''}`}
                onClick={() => updateEngineMode('spatial')}
              >
                <span className="tab-icon">🧭</span>
                <div className="tab-copy">
                  <strong>Spatial Position (Phase 7)</strong>
                  <span>2D sound-source positioning with acoustic distance falloff</span>
                </div>
              </button>
              <button
                className={`mode-tab-btn ${waveStatus.engineMode === 'pattern' ? 'active' : ''}`}
                onClick={() => updateEngineMode('pattern')}
              >
                <span className="tab-icon">🌊</span>
                <div className="tab-copy">
                  <strong>Pattern Wave (Phase 6)</strong>
                  <span>Circular, Ping-Pong, Pulse, and Chase traveling waves</span>
                </div>
              </button>
            </div>

            {/* MODE 1: ADVANCED MOTION ENGINE (Phase 8) */}
            {waveStatus.engineMode === 'motion' && (
              <div className="motion-engine-content">
                {/* Live Motion Status Card */}
                {waveStatus.enabled && engineActive && (
                  <div className="wave-live-card motion-live-card">
                    <div className="wave-live-header">
                      <span className="wave-pulse-dot" />
                      <div className="wave-live-title-wrap">
                        <h4>Speaker Motion Active</h4>
                        <div className="wave-live-badges">
                          <span className="badge wave-badge">
                            Mode: <strong>{(waveStatus.motion?.motionMode || waveStatus.motionMode || 'SMOOTH').toUpperCase()}</strong>
                          </span>
                          <span className="badge wave-badge">
                            Curve: <strong>{(waveStatus.motion?.motionCurve || waveStatus.motionCurve || 'equal_power').replace('_', ' ').toUpperCase()}</strong>
                          </span>
                          <span className="badge wave-badge">
                            Speed: <strong>{(waveStatus.motion?.motionSpeed || waveStatus.motionSpeed || 0.5).toFixed(2)} Hz</strong>
                          </span>
                          <span className="badge wave-badge">
                            Master: <strong>{waveStatus.masterMuted ? '0%' : `${waveStatus.masterVolume ?? 100}%`}</strong>
                          </span>
                        </div>
                      </div>
                    </div>

                    <div className="wave-movement-banner">
                      <div className="wave-movement-desc">
                        <span className="movement-label">Current Trajectory:</span>
                        <strong className="movement-text">
                          {waveStatus.motion?.currentMotionState?.description || waveStatus.currentTransition?.description || 'Motion in progress'}
                        </strong>
                      </div>
                      <div className="motion-progress-banner">
                        <div className="motion-progress-track">
                          <div
                            className="motion-progress-fill"
                            style={{
                              width: `${Math.max(0, Math.min(100, (waveStatus.motion?.currentMotionState?.progress ?? waveStatus.currentTransition?.progress ?? 0) * 100))}%`
                            }}
                          />
                        </div>
                      </div>
                    </div>

                    {/* Real-time Branch Gain Levels */}
                    <div className="wave-meters-grid">
                      {(waveStatus.branchGains || []).map((bg) => (
                        <div className="wave-meter-item" key={bg.sinkId}>
                          <div className="wave-meter-label">
                            <span>{bg.sinkName} {bg.muted ? '(Muted)' : ''}</span>
                            <span className="wave-meter-val">{bg.gainPercent}%</span>
                          </div>
                          <div className="wave-meter-bar-track">
                            <div
                              className="wave-meter-bar-fill"
                              style={{ width: `${Math.max(0, Math.min(100, bg.gainPercent))}%` }}
                            />
                          </div>
                        </div>
                      ))}
                    </div>
                  </div>
                )}

                {/* Motion Modes Grid */}
                <div className="wave-patterns-section">
                  <label className="wave-control-label">
                    <span>Motion Traversal Mode</span>
                  </label>
                  <div className="wave-pattern-grid">
                    <button
                      className={`pattern-card-btn ${(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'smooth' ? 'active' : ''}`}
                      onClick={() => updateMotionMode('smooth')}
                    >
                      <div className="pattern-title">Smooth Travel</div>
                      <div className="pattern-diagram">A ⇄ B ⇄ C ⇄ A (Continuous)</div>
                      <div className="pattern-caption">Continuous equal-power crossfade panning seamlessly between neighbor speakers.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'sequential' ? 'active' : ''}`}
                      onClick={() => updateMotionMode('sequential')}
                    >
                      <div className="pattern-title">Sequential Travel</div>
                      <div className="pattern-diagram">A [Dwell] → B [Dwell] → C</div>
                      <div className="pattern-caption">Holds audio on each speaker for a set dwell time before quickly switching.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'circular' ? 'active' : ''}`}
                      onClick={() => updateMotionMode('circular')}
                    >
                      <div className="pattern-title">Circular Loop</div>
                      <div className="pattern-diagram">Closed Polygon Traversal</div>
                      <div className="pattern-caption">Continuous loop following speaker order around the perimeter.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'bounce' ? 'active' : ''}`}
                      onClick={() => updateMotionMode('bounce')}
                    >
                      <div className="pattern-title">Bounce</div>
                      <div className="pattern-diagram">A → B → C → B → A</div>
                      <div className="pattern-caption">Bidirectional travel reflecting smoothly at endpoint boundaries.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'shuffle' ? 'active' : ''}`}
                      onClick={() => updateMotionMode('shuffle')}
                    >
                      <div className="pattern-title">Shuffle</div>
                      <div className="pattern-diagram">Randomized (Non-Repeating)</div>
                      <div className="pattern-caption">Smooth transitions across randomly selected speakers without immediate repeats.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'spatial_path' ? 'active' : ''}`}
                      onClick={() => updateMotionMode('spatial_path')}
                    >
                      <div className="pattern-title">Spatial Path</div>
                      <div className="pattern-diagram">2D Vector Trajectory</div>
                      <div className="pattern-caption">Trajectory motion (Figure-8, Lissajous, Waypoints, Spiral) in 2D space.</div>
                    </button>
                  </div>
                </div>

                {/* Motion Parameter Controls */}
                <div className="wave-controls-grid">
                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Direction</span>
                    </label>
                    <div className="wave-direction-toggle">
                      <button
                        className={`direction-btn ${waveStatus.direction === 'forward' ? 'active' : ''}`}
                        onClick={() => updateWaveDirection('forward')}
                      >
                        Forward ↻
                      </button>
                      <button
                        className={`direction-btn ${waveStatus.direction === 'reverse' ? 'active' : ''}`}
                        onClick={() => updateWaveDirection('reverse')}
                      >
                        Reverse ↺
                      </button>
                    </div>
                  </div>

                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Speed: <strong>{(waveStatus.motion?.motionSpeed || waveStatus.motionSpeed || 0.5).toFixed(2)} cycles/sec</strong></span>
                    </label>
                    <div className="wave-slider-row">
                      <span className="slider-edge-label">0.1x</span>
                      <input
                        type="range"
                        min="0.1"
                        max="3.0"
                        step="0.05"
                        value={waveStatus.motion?.motionSpeed || waveStatus.motionSpeed || 0.5}
                        onChange={(e) => updateMotionSpeed(parseFloat(e.target.value))}
                      />
                      <span className="slider-edge-label">3.0x</span>
                    </div>
                  </div>

                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Crossfade Curve</span>
                    </label>
                    <div className="curve-toggle-group">
                      <button
                        className={`curve-btn ${(waveStatus.motion?.motionCurve || waveStatus.motionCurve || 'equal_power') === 'equal_power' ? 'active' : ''}`}
                        onClick={() => updateMotionCurve('equal_power')}
                      >
                        Equal-Power
                      </button>
                      <button
                        className={`curve-btn ${(waveStatus.motion?.motionCurve || waveStatus.motionCurve) === 'linear' ? 'active' : ''}`}
                        onClick={() => updateMotionCurve('linear')}
                      >
                        Linear
                      </button>
                      <button
                        className={`curve-btn ${(waveStatus.motion?.motionCurve || waveStatus.motionCurve) === 'smoothstep' ? 'active' : ''}`}
                        onClick={() => updateMotionCurve('smoothstep')}
                      >
                        Smoothstep
                      </button>
                    </div>
                  </div>

                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Intensity: <strong>{Math.round((waveStatus.intensity ?? 1.0) * 100)}%</strong></span>
                    </label>
                    <div className="wave-slider-row">
                      <span className="slider-edge-label">0% (Equal)</span>
                      <input
                        type="range"
                        min="0"
                        max="1.0"
                        step="0.05"
                        value={waveStatus.intensity ?? 1.0}
                        onChange={(e) => updateWaveIntensity(parseFloat(e.target.value))}
                      />
                      <span className="slider-edge-label">100% (Full)</span>
                    </div>
                  </div>

                  {(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'sequential' && (
                    <div className="wave-control-card">
                      <label className="wave-control-label">
                        <span>Sequential Dwell Ratio: <strong>{Math.round((waveStatus.motion?.motionDwellRatio || waveStatus.motionDwellRatio || 0.6) * 100)}%</strong></span>
                      </label>
                      <div className="wave-slider-row">
                        <span className="slider-edge-label">0% (Instant Switch)</span>
                        <input
                          type="range"
                          min="0.0"
                          max="0.95"
                          step="0.05"
                          value={waveStatus.motion?.motionDwellRatio || waveStatus.motionDwellRatio || 0.6}
                          onChange={(e) => updateMotionDwell(parseFloat(e.target.value))}
                        />
                        <span className="slider-edge-label">95% (Long Hold)</span>
                      </div>
                    </div>
                  )}

                  {(waveStatus.motion?.motionMode || waveStatus.motionMode) === 'spatial_path' && (
                    <div className="wave-control-card">
                      <label className="wave-control-label">
                        <span>2D Trajectory Geometry</span>
                      </label>
                      <div className="curve-toggle-group">
                        <button
                          className={`curve-btn ${(waveStatus.motion?.spatialPathType || waveStatus.spatialPathType || 'figure8') === 'figure8' ? 'active' : ''}`}
                          onClick={() => updateSpatialPathType('figure8')}
                        >
                          Figure-8 (∞)
                        </button>
                        <button
                          className={`curve-btn ${(waveStatus.motion?.spatialPathType || waveStatus.spatialPathType) === 'waypoints' ? 'active' : ''}`}
                          onClick={() => updateSpatialPathType('waypoints')}
                        >
                          Waypoints
                        </button>
                        <button
                          className={`curve-btn ${(waveStatus.motion?.spatialPathType || waveStatus.spatialPathType) === 'lissajous' ? 'active' : ''}`}
                          onClick={() => updateSpatialPathType('lissajous')}
                        >
                          Lissajous
                        </button>
                        <button
                          className={`curve-btn ${(waveStatus.motion?.spatialPathType || waveStatus.spatialPathType) === 'spiral' ? 'active' : ''}`}
                          onClick={() => updateSpatialPathType('spiral')}
                        >
                          Spiral
                        </button>
                      </div>
                    </div>
                  )}

                  {/* Motion Speaker Sequence Order */}
                  <div className="wave-control-card wave-order-card">
                    <label className="wave-control-label">
                      <span>Motion Sequence Traversal ({(waveStatus.motion?.order || waveStatus.order || []).length} speakers)</span>
                    </label>
                    <div className="wave-order-list">
                      {(waveStatus.motion?.order || waveStatus.order || []).length === 0 && (
                        <p className="empty-hint">Start the Multi-Speaker Engine with ≥ 2 speakers to configure motion sequence.</p>
                      )}
                      {(waveStatus.motion?.order || waveStatus.order || []).map((sinkId, idx) => {
                        const branch = branchMap.get(sinkId);
                        const name = branch?.sinkName || sinkId;
                        return (
                          <div className="wave-order-item" key={sinkId}>
                            <span className="order-idx">{idx + 1}.</span>
                            <span className="order-name">{name}</span>
                            <div className="order-actions">
                              <button
                                className="order-btn"
                                disabled={idx === 0}
                                onClick={() => moveSpeakerInMotionOrder(idx, -1)}
                                title="Move earlier in traversal"
                              >
                                ↑
                              </button>
                              <button
                                className="order-btn"
                                disabled={idx === (waveStatus.motion?.order || waveStatus.order || []).length - 1}
                                onClick={() => moveSpeakerInMotionOrder(idx, 1)}
                                title="Move later in traversal"
                              >
                                ↓
                              </button>
                            </div>
                          </div>
                        );
                      })}
                    </div>
                  </div>
                </div>
              </div>
            )}

            {/* MODE 2: SPATIAL POSITION MODE (Phase 7) */}
            {waveStatus.engineMode === 'spatial' && (
              <div className="spatial-engine-content">
                {/* Spatial Live Status Banner */}
                {waveStatus.enabled && engineActive && (
                  <div className="wave-live-card spatial-live-card">
                    <div className="wave-live-header">
                      <span className="wave-pulse-dot" />
                      <div className="wave-live-title-wrap">
                        <h4>Spatial Positioning Active</h4>
                        <div className="wave-live-badges">
                          <span className="badge wave-badge">
                            Mode: <strong>{waveStatus.spatialPattern?.toUpperCase() || 'ORBIT'}</strong>
                          </span>
                          <span className="badge wave-badge">
                            Source: <strong>({displaySourceX >= 0 ? '+' : ''}{displaySourceX.toFixed(2)}, {displaySourceY >= 0 ? '+' : ''}{displaySourceY.toFixed(2)})</strong>
                          </span>
                          <span className="badge wave-badge">
                            Speed: <strong>{waveStatus.speed.toFixed(2)} Hz</strong>
                          </span>
                          <span className="badge wave-badge">
                            Master: <strong>{waveStatus.masterMuted ? '0%' : `${waveStatus.masterVolume ?? 100}%`}</strong>
                          </span>
                        </div>
                      </div>
                    </div>

                    <div className="wave-movement-banner">
                      <div className="wave-movement-desc">
                        <span className="movement-label">Telemetry:</span>
                        <strong className="movement-text">
                          {waveStatus.currentSpatialState?.description || `Source at (${displaySourceX.toFixed(2)}, ${displaySourceY.toFixed(2)})`}
                        </strong>
                      </div>
                    </div>

                    {/* Real-time Branch Gain Levels */}
                    <div className="wave-meters-grid">
                      {(waveStatus.branchGains || []).map((bg) => (
                        <div className="wave-meter-item" key={bg.sinkId}>
                          <div className="wave-meter-label">
                            <span>{bg.sinkName} {bg.muted ? '(Muted)' : ''}</span>
                            <span className="wave-meter-val">{bg.gainPercent}%</span>
                          </div>
                          <div className="wave-meter-bar-track">
                            <div
                              className="wave-meter-bar-fill"
                              style={{ width: `${Math.max(0, Math.min(100, bg.gainPercent))}%` }}
                            />
                          </div>
                        </div>
                      ))}
                    </div>
                  </div>
                )}

                {/* Interactive 2D Room Canvas */}
                <div className="room-canvas-container">
                  <div className="canvas-header">
                    <h3>2D Virtual Room Canvas</h3>
                    <span className="canvas-hint">
                      {waveStatus.spatialPattern === 'static'
                        ? 'Click or drag sound source ◆ to move audio in real-time. Drag speaker nodes ● to arrange room layout.'
                        : 'Live backend trajectory visualization. Drag speaker nodes ● to adjust acoustic positions.'}
                    </span>
                  </div>

                  <div className="room-canvas-box">
                    <svg
                      className="room-svg"
                      viewBox="0 0 340 340"
                      ref={svgCanvasRef}
                      onPointerDown={handleCanvasPointerDown}
                      onPointerMove={handleCanvasPointerMove}
                    >
                      <rect x="20" y="20" width="300" height="300" rx="14" className="room-boundary" />

                      <circle cx="170" cy="170" r="125" className="room-ring room-ring-outer" />
                      <circle cx="170" cy="170" r="93.75" className="room-ring" />
                      <circle cx="170" cy="170" r="62.5" className="room-ring" />
                      <circle cx="170" cy="170" r="31.25" className="room-ring" />

                      <line x1="20" y1="170" x2="320" y2="170" className="room-axis" />
                      <line x1="170" y1="20" x2="170" y2="320" className="room-axis" />
                      <circle cx="170" cy="170" r="3.5" className="room-origin" />

                      <text x="170" y="15" className="room-axis-label front-label" textAnchor="middle">FRONT (Top)</text>
                      <text x="170" y="333" className="room-axis-label back-label" textAnchor="middle">BACK (Bottom)</text>
                      <text x="12" y="174" className="room-axis-label left-label" textAnchor="middle">LEFT</text>
                      <text x="328" y="174" className="room-axis-label right-label" textAnchor="middle">RIGHT</text>

                      {waveStatus.spatialPattern === 'orbit' && (
                        <circle
                          cx="170"
                          cy="170"
                          r={Math.max(10, (waveStatus.orbitRadius || 0.7) * 125)}
                          className="room-trajectory-orbit"
                        />
                      )}
                      {waveStatus.spatialPattern === 'horizontal_sweep' && (
                        <line
                          x1="45"
                          y1={toSvgY(waveStatus.sourceY)}
                          x2="295"
                          y2={toSvgY(waveStatus.sourceY)}
                          className="room-trajectory-sweep"
                        />
                      )}
                      {waveStatus.spatialPattern === 'vertical_sweep' && (
                        <line
                          x1={toSvgX(waveStatus.sourceX)}
                          y1="45"
                          x2={toSvgX(waveStatus.sourceX)}
                          y2="295"
                          className="room-trajectory-sweep"
                        />
                      )}

                      {/* Distance Vectors */}
                      {(engineSession?.branches || []).filter((b) => b.state === 'active').map((branch) => {
                        const pos = (waveStatus.speakerPositions || {})[branch.sinkId] || { x: 0, y: 0 };
                        const spkSvgX = toSvgX(pos.x);
                        const spkSvgY = toSvgY(pos.y);
                        const srcSvgX = toSvgX(displaySourceX);
                        const srcSvgY = toSvgY(displaySourceY);
                        const bg = (waveStatus.branchGains || []).find((g) => g.sinkId === branch.sinkId);
                        const gainPct = bg?.gainPercent ?? 100;
                        return (
                          <line
                            key={`line-${branch.sinkId}`}
                            x1={srcSvgX}
                            y1={srcSvgY}
                            x2={spkSvgX}
                            y2={spkSvgY}
                            className="room-vector-line"
                            style={{ opacity: 0.15 + 0.65 * (gainPct / 100) }}
                          />
                        );
                      })}

                      {/* Speaker Nodes */}
                      {(engineSession?.branches || []).filter((b) => b.state === 'active').map((branch, idx) => {
                        const pos = (waveStatus.speakerPositions || {})[branch.sinkId] || { x: 0, y: 0 };
                        const svgX = toSvgX(pos.x);
                        const svgY = toSvgY(pos.y);
                        const bg = (waveStatus.branchGains || []).find((g) => g.sinkId === branch.sinkId);
                        const gainPct = bg?.gainPercent ?? 100;

                        return (
                          <g
                            key={branch.sinkId}
                            className="room-speaker-group"
                            onPointerDown={(e) => {
                              e.stopPropagation();
                              setDraggingTarget(branch.sinkId);
                            }}
                          >
                            <circle
                              cx={svgX}
                              cy={svgY}
                              r={12 + 10 * (gainPct / 100)}
                              className="speaker-halo"
                              style={{ opacity: 0.2 + 0.5 * (gainPct / 100) }}
                            />
                            <circle cx={svgX} cy={svgY} r="10" className="speaker-node-dot" />
                            <text x={svgX} y={svgY + 3.5} className="speaker-node-letter" textAnchor="middle">
                              {String.fromCharCode(65 + (idx % 26))}
                            </text>
                            <text x={svgX} y={svgY - 14} className="speaker-svg-label" textAnchor="middle">
                              {branch.sinkName.length > 14 ? `${branch.sinkName.slice(0, 12)}…` : branch.sinkName} ({gainPct}%)
                            </text>
                          </g>
                        );
                      })}

                      {/* Virtual Sound Source Puck */}
                      <g
                        className="room-source-group"
                        onPointerDown={(e) => {
                          e.stopPropagation();
                          setDraggingTarget('source');
                        }}
                      >
                        <circle cx={toSvgX(displaySourceX)} cy={toSvgY(displaySourceY)} r="20" className="source-pulse-ring" />
                        <circle cx={toSvgX(displaySourceX)} cy={toSvgY(displaySourceY)} r="12" className="source-pulse-core" />
                        <rect
                          x={toSvgX(displaySourceX) - 7}
                          y={toSvgY(displaySourceY) - 7}
                          width="14"
                          height="14"
                          className="source-diamond"
                          transform={`rotate(45 ${toSvgX(displaySourceX)} ${toSvgY(displaySourceY)})`}
                        />
                        <text
                          x={toSvgX(displaySourceX)}
                          y={toSvgY(displaySourceY) + 24}
                          className="source-svg-label"
                          textAnchor="middle"
                        >
                          Sound Source ◆
                        </text>
                      </g>
                    </svg>
                  </div>
                </div>

                {/* Spatial Pattern Selector */}
                <div className="wave-patterns-section">
                  <label className="wave-control-label">
                    <span>Spatial Movement Mode</span>
                  </label>
                  <div className="wave-pattern-grid">
                    <button
                      className={`pattern-card-btn ${waveStatus.spatialPattern === 'orbit' ? 'active' : ''}`}
                      onClick={() => updateSpatialPattern('orbit')}
                    >
                      <div className="pattern-title">Orbit</div>
                      <div className="pattern-diagram">Circular 2D Trajectory</div>
                      <div className="pattern-caption">Continuous circular rotation around center (0, 0) with configurable radius.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${waveStatus.spatialPattern === 'static' ? 'active' : ''}`}
                      onClick={() => updateSpatialPattern('static')}
                    >
                      <div className="pattern-title">Static Position</div>
                      <div className="pattern-diagram">Manual Drag & Drop (X, Y)</div>
                      <div className="pattern-caption">Fixed source coordinate. Drag sound source ◆ directly on the canvas.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${waveStatus.spatialPattern === 'horizontal_sweep' ? 'active' : ''}`}
                      onClick={() => updateSpatialPattern('horizontal_sweep')}
                    >
                      <div className="pattern-title">Horizontal Sweep</div>
                      <div className="pattern-diagram">Left ⇄ Right Sweep</div>
                      <div className="pattern-caption">Oscillates horizontally across X [-1, +1] at a configurable Y position.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${waveStatus.spatialPattern === 'vertical_sweep' ? 'active' : ''}`}
                      onClick={() => updateSpatialPattern('vertical_sweep')}
                    >
                      <div className="pattern-title">Vertical Sweep</div>
                      <div className="pattern-diagram">Back ⇄ Front Sweep</div>
                      <div className="pattern-caption">Oscillates vertically across Y [-1, +1] at a configurable X position.</div>
                    </button>
                  </div>
                </div>

                {/* Spatial Configuration Controls */}
                <div className="wave-controls-grid">
                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Spatial Falloff: <strong>{Math.round((waveStatus.spatialFalloff ?? 0.5) * 100)}%</strong></span>
                    </label>
                    <div className="wave-slider-row">
                      <span className="slider-edge-label">Soft (0%)</span>
                      <input
                        type="range"
                        min="0.0"
                        max="1.0"
                        step="0.05"
                        value={waveStatus.spatialFalloff ?? 0.5}
                        onChange={(e) => updateSpatialFalloff(parseFloat(e.target.value))}
                      />
                      <span className="slider-edge-label">Focused (100%)</span>
                    </div>
                  </div>

                  {waveStatus.spatialPattern !== 'static' && (
                    <div className="wave-control-card">
                      <label className="wave-control-label">
                        <span>Speed: <strong>{waveStatus.speed.toFixed(2)} cycles/sec</strong></span>
                      </label>
                      <div className="wave-slider-row">
                        <span className="slider-edge-label">0.25x</span>
                        <input
                          type="range"
                          min="0.25"
                          max="2.0"
                          step="0.05"
                          value={waveStatus.speed}
                          onChange={(e) => updateWaveSpeed(parseFloat(e.target.value))}
                        />
                        <span className="slider-edge-label">2.0x</span>
                      </div>
                    </div>
                  )}

                  {waveStatus.spatialPattern !== 'static' && (
                    <div className="wave-control-card">
                      <label className="wave-control-label">
                        <span>Direction</span>
                      </label>
                      <div className="wave-direction-toggle">
                        <button
                          className={`direction-btn ${waveStatus.direction === 'forward' ? 'active' : ''}`}
                          onClick={() => updateWaveDirection('forward')}
                        >
                          Forward
                        </button>
                        <button
                          className={`direction-btn ${waveStatus.direction === 'reverse' ? 'active' : ''}`}
                          onClick={() => updateWaveDirection('reverse')}
                        >
                          Reverse
                        </button>
                      </div>
                    </div>
                  )}

                  {waveStatus.spatialPattern === 'orbit' && (
                    <div className="wave-control-card wave-dynamic-card">
                      <label className="wave-control-label">
                        <span>Orbit Radius: <strong>{(waveStatus.orbitRadius ?? 0.7).toFixed(2)}</strong></span>
                      </label>
                      <div className="wave-slider-row">
                        <span className="slider-edge-label">0.10</span>
                        <input
                          type="range"
                          min="0.1"
                          max="1.0"
                          step="0.05"
                          value={waveStatus.orbitRadius ?? 0.7}
                          onChange={(e) => updateOrbitRadius(parseFloat(e.target.value))}
                        />
                        <span className="slider-edge-label">1.00</span>
                      </div>
                    </div>
                  )}

                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Intensity: <strong>{Math.round(waveStatus.intensity * 100)}%</strong></span>
                    </label>
                    <div className="wave-slider-row">
                      <span className="slider-edge-label">0% (Equal)</span>
                      <input
                        type="range"
                        min="0"
                        max="1.0"
                        step="0.05"
                        value={waveStatus.intensity}
                        onChange={(e) => updateWaveIntensity(parseFloat(e.target.value))}
                      />
                      <span className="slider-edge-label">100% (Full)</span>
                    </div>
                  </div>
                </div>

                {/* Speaker Coordinates Layout Table */}
                <div className="speaker-layout-table-card">
                  <div className="layout-header">
                    <div>
                      <h4>Speaker Room Coordinates</h4>
                      <p>Customize physical speaker positions in your 2D room layout.</p>
                    </div>
                    <button className="button secondary reset-btn" onClick={resetSpeakerPositions}>
                      Reset Default Layout
                    </button>
                  </div>
                  <div className="speaker-coords-grid">
                    {(engineSession?.branches || []).filter((b) => b.state === 'active').map((branch, idx) => {
                      const pos = (waveStatus.speakerPositions || {})[branch.sinkId] || { x: 0, y: 0 };
                      return (
                        <div className="speaker-coord-item" key={branch.sinkId}>
                          <div className="coord-item-header">
                            <span className="spk-badge-letter">{String.fromCharCode(65 + (idx % 26))}</span>
                            <span className="spk-item-name">{branch.sinkName}</span>
                          </div>
                          <div className="coord-sliders-pair">
                            <div className="coord-slider-group">
                              <span className="coord-axis-tag">X</span>
                              <input
                                type="range"
                                min="-1.0"
                                max="1.0"
                                step="0.05"
                                value={pos.x}
                                onChange={(e) => updateSpeakerPosition(branch.sinkId, parseFloat(e.target.value), pos.y)}
                              />
                              <span className="coord-val-pill">{pos.x >= 0 ? `+${pos.x.toFixed(2)}` : pos.x.toFixed(2)}</span>
                            </div>
                            <div className="coord-slider-group">
                              <span className="coord-axis-tag">Y</span>
                              <input
                                type="range"
                                min="-1.0"
                                max="1.0"
                                step="0.05"
                                value={pos.y}
                                onChange={(e) => updateSpeakerPosition(branch.sinkId, pos.x, parseFloat(e.target.value))}
                              />
                              <span className="coord-val-pill">{pos.y >= 0 ? `+${pos.y.toFixed(2)}` : pos.y.toFixed(2)}</span>
                            </div>
                          </div>
                        </div>
                      );
                    })}
                  </div>
                </div>
              </div>
            )}

            {/* MODE B: PATTERN WAVE MODE (Phase 6) */}
            {waveStatus.engineMode === 'pattern' && (
              <div className="pattern-engine-content">
                {waveStatus.enabled && engineActive && (
                  <div className="wave-live-card">
                    <div className="wave-live-header">
                      <span className="wave-pulse-dot" />
                      <div className="wave-live-title-wrap">
                        <h4>Wave Pattern Active</h4>
                        <div className="wave-live-badges">
                          <span className="badge wave-badge">
                            Pattern: <strong>{waveStatus.pattern.toUpperCase()}</strong>
                          </span>
                          <span className="badge wave-badge">
                            Direction: <strong>{waveStatus.direction === 'reverse' ? 'REVERSE' : 'FORWARD'}</strong>
                          </span>
                          <span className="badge wave-badge">
                            Speed: <strong>{waveStatus.speed.toFixed(2)} Hz</strong>
                          </span>
                          <span className="badge wave-badge">
                            Master: <strong>{waveStatus.masterMuted ? '0%' : `${waveStatus.masterVolume ?? 100}%`}</strong>
                          </span>
                        </div>
                      </div>
                    </div>

                    {waveStatus.currentTransition && (
                      <div className="wave-movement-banner">
                        <div className="wave-movement-desc">
                          <span className="movement-label">Current Movement:</span>
                          <strong className="movement-text">
                            {waveStatus.currentTransition.description || `${waveStatus.currentTransition.fromSinkName} → ${waveStatus.currentTransition.toSinkName}`}
                          </strong>
                        </div>
                      </div>
                    )}

                    {/* Real-time Branch Gain Levels */}
                    <div className="wave-meters-grid">
                      {(waveStatus.branchGains || []).map((bg) => (
                        <div className="wave-meter-item" key={bg.sinkId}>
                          <div className="wave-meter-label">
                            <span>{bg.sinkName} {bg.muted ? '(Muted)' : ''}</span>
                            <span className="wave-meter-val">{bg.gainPercent}%</span>
                          </div>
                          <div className="wave-meter-bar-track">
                            <div
                              className="wave-meter-bar-fill"
                              style={{ width: `${Math.max(0, Math.min(100, bg.gainPercent))}%` }}
                            />
                          </div>
                        </div>
                      ))}
                    </div>
                  </div>
                )}

                {/* Wave Pattern Selector */}
                <div className="wave-patterns-section">
                  <label className="wave-control-label">
                    <span>Movement Pattern</span>
                  </label>
                  <div className="wave-pattern-grid">
                    <button
                      className={`pattern-card-btn ${waveStatus.pattern === 'circular' ? 'active' : ''}`}
                      onClick={() => updateWavePattern('circular')}
                    >
                      <div className="pattern-title">Circular</div>
                      <div className="pattern-diagram">A → B → C → D → A</div>
                      <div className="pattern-caption">Smooth continuous cyclic rotation around all ordered speakers.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${waveStatus.pattern === 'pingpong' ? 'active' : ''}`}
                      onClick={() => updateWavePattern('pingpong')}
                    >
                      <div className="pattern-title">Ping-Pong</div>
                      <div className="pattern-diagram">A → B → C → D → C → B → A</div>
                      <div className="pattern-caption">Natural boundary reflection reversing direction smoothly at endpoints.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${waveStatus.pattern === 'pulse' ? 'active' : ''}`}
                      onClick={() => updateWavePattern('pulse')}
                    >
                      <div className="pattern-title">Pulse</div>
                      <div className="pattern-diagram">A+B → B+C → C+D → D+A</div>
                      <div className="pattern-caption">Configurable moving multi-speaker window traveling continuously.</div>
                    </button>

                    <button
                      className={`pattern-card-btn ${waveStatus.pattern === 'chase' ? 'active' : ''}`}
                      onClick={() => updateWavePattern('chase')}
                    >
                      <div className="pattern-title">Chase</div>
                      <div className="pattern-diagram">A (100%) → B (60%) → C (25%)</div>
                      <div className="pattern-caption">Strong focal leader with trailing followers decaying via falloff.</div>
                    </button>
                  </div>
                </div>

                {/* Wave Configuration Controls */}
                <div className="wave-controls-grid">
                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Direction</span>
                    </label>
                    <div className="wave-direction-toggle">
                      <button
                        className={`direction-btn ${waveStatus.direction === 'forward' ? 'active' : ''}`}
                        onClick={() => updateWaveDirection('forward')}
                      >
                        Forward
                      </button>
                      <button
                        className={`direction-btn ${waveStatus.direction === 'reverse' ? 'active' : ''}`}
                        onClick={() => updateWaveDirection('reverse')}
                      >
                        Reverse
                      </button>
                    </div>
                  </div>

                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Speed: <strong>{waveStatus.speed.toFixed(2)} cycles/sec</strong></span>
                    </label>
                    <div className="wave-slider-row">
                      <span className="slider-edge-label">0.25x</span>
                      <input
                        type="range"
                        min="0.25"
                        max="2.0"
                        step="0.05"
                        value={waveStatus.speed}
                        onChange={(e) => updateWaveSpeed(parseFloat(e.target.value))}
                      />
                      <span className="slider-edge-label">2.0x</span>
                    </div>
                  </div>

                  <div className="wave-control-card">
                    <label className="wave-control-label">
                      <span>Intensity: <strong>{Math.round(waveStatus.intensity * 100)}%</strong></span>
                    </label>
                    <div className="wave-slider-row">
                      <span className="slider-edge-label">0% (Equal)</span>
                      <input
                        type="range"
                        min="0"
                        max="1.0"
                        step="0.05"
                        value={waveStatus.intensity}
                        onChange={(e) => updateWaveIntensity(parseFloat(e.target.value))}
                      />
                      <span className="slider-edge-label">100% (Full)</span>
                    </div>
                  </div>

                  {waveStatus.pattern === 'pulse' && (
                    <div className="wave-control-card wave-dynamic-card">
                      <label className="wave-control-label">
                        <span>Pulse Width: <strong>{waveStatus.pulseWidth || 2} speakers</strong></span>
                      </label>
                      <div className="wave-slider-row">
                        <span className="slider-edge-label">1 spk</span>
                        <input
                          type="range"
                          min="1"
                          max={Math.max(2, waveStatus.activeCount || 4)}
                          step="1"
                          value={waveStatus.pulseWidth || 2}
                          onChange={(e) => updateWavePulseWidth(parseInt(e.target.value, 10))}
                        />
                        <span className="slider-edge-label">{Math.max(2, waveStatus.activeCount || 4)} spks</span>
                      </div>
                    </div>
                  )}

                  {waveStatus.pattern === 'chase' && (
                    <div className="wave-control-card wave-dynamic-card">
                      <label className="wave-control-label">
                        <span>Chase Falloff: <strong>{Math.round((waveStatus.chaseFalloff ?? 0.5) * 100)}%</strong></span>
                      </label>
                      <div className="wave-slider-row">
                        <span className="slider-edge-label">0% (Broad)</span>
                        <input
                          type="range"
                          min="0.0"
                          max="1.0"
                          step="0.05"
                          value={waveStatus.chaseFalloff ?? 0.5}
                          onChange={(e) => updateWaveChaseFalloff(parseFloat(e.target.value))}
                        />
                        <span className="slider-edge-label">100% (Focused)</span>
                      </div>
                    </div>
                  )}

                  {/* Speaker Order Sequence */}
                  <div className="wave-control-card wave-order-card">
                    <label className="wave-control-label">
                      <span>Wave Order ({waveStatus.order?.length || 0} speakers in rotation)</span>
                    </label>
                    <div className="wave-order-list">
                      {(waveStatus.order || []).length === 0 && (
                        <p className="empty-hint">Start the Multi-Speaker Engine with ≥ 2 speakers to configure wave order.</p>
                      )}
                      {(waveStatus.order || []).map((sinkId, idx) => {
                        const branch = branchMap.get(sinkId);
                        const name = branch?.sinkName || sinkId;
                        return (
                          <div className="wave-order-item" key={sinkId}>
                            <span className="order-idx">{idx + 1}.</span>
                            <span className="order-name">{name}</span>
                            <div className="order-actions">
                              <button
                                className="order-btn"
                                disabled={idx === 0}
                                onClick={() => moveSpeakerInOrder(idx, -1)}
                                title="Move earlier in rotation"
                              >
                                ↑
                              </button>
                              <button
                                className="order-btn"
                                disabled={idx === (waveStatus.order?.length || 0) - 1}
                                onClick={() => moveSpeakerInOrder(idx, 1)}
                                title="Move later in rotation"
                              >
                                ↓
                              </button>
                            </div>
                          </div>
                        );
                      })}
                    </div>
                  </div>
                </div>
              </div>
            )}

            {/* Wave & Spatial Master Action Bar */}
            <div className="stream-actions wave-footer-actions">
              <button
                className={`button ${waveStatus.enabled ? 'secondary' : 'primary'}`}
                onClick={toggleWaveEnabled}
                disabled={waveAction}
              >
                {waveAction
                  ? 'Updating…'
                  : waveStatus.enabled
                  ? 'Disable Audio Movement (Restore Equal Gain)'
                  : waveStatus.engineMode === 'spatial'
                  ? 'Enable Spatial Audio'
                  : 'Enable Spatial Wave'}
              </button>
            </div>
          </div>
        </section>
      )}

      {/* ========================================================================= */}
      {/* TAB 4: ENGINE TOPOLOGY GRAPH (Phase 4 Multi-Speaker Engine)               */}
      {/* ========================================================================= */}
      {activeTab === 'topology' && (
        <section className="panel engine-panel" aria-labelledby="multi-engine-heading">
          <div className="panel-heading">
            <div>
              <h2 id="multi-engine-heading">Multi-Speaker Engine Graph</h2>
              <p>Temporary 1-to-N fan-out engine using an isolated ingress virtual sink, tap, and independent branches.</p>
            </div>
            <span className={`status ${engineActive ? engineSession?.state || 'active' : 'idle'}`}>
              {engineActive ? engineSession?.state || 'running' : 'idle'}
            </span>
          </div>

          {engineSession?.message && <p className="engine-message">{engineSession.message}</p>}

          <div className="engine-body">
            <div className="engine-speakers-header">
              <h3>Target Physical Outputs</h3>
              <span className="count">
                {selectedEngineSinkIds.size} of {engineStatus?.outputs?.length || 0} selected
              </span>
            </div>

            <div className="device-list engine-sink-list">
              {(engineStatus?.outputs || []).length === 0 && (
                <p className="empty">No physical audio outputs available.</p>
              )}
              {(engineStatus?.outputs || []).map((output) => (
                <label
                  className={`device-card engine-device-card ${selectedEngineSinkIds.has(output.id) ? 'selected' : ''}`}
                  key={output.id}
                >
                  <input
                    type="checkbox"
                    checked={selectedEngineSinkIds.has(output.id)}
                    onChange={() => toggleEngineSink(output.id)}
                    disabled={engineActive || engineAction}
                  />
                  <span className="device-copy">
                    <span className="device-name">{output.name}</span>
                    <span className="device-id">{output.technicalName}</span>
                  </span>
                  <span className="status active">Connected</span>
                  {output.isDefault && <span className="badge">Default</span>}
                </label>
              ))}
            </div>

            <div className="engine-stream-summary">
              <span className="summary-label">Target Application Stream:</span>
              <div className="engine-stream-picker-wrap">
                {engineActive && engineSession ? (
                  <div className="engine-active-stream-pill">
                    <span className="stream-app-name">{engineSession.streamName || 'Active Session Stream'}</span>
                    <span className="stream-id-tag">(Stream {engineSession.streamId})</span>
                    <span className={`status-pill ${engineSession.streamActive !== false ? 'playing' : 'paused'}`}>
                      {engineSession.streamActive !== false ? 'playing' : 'paused'}
                    </span>
                  </div>
                ) : streams.length > 0 ? (
                  <select
                    className="pavu-select engine-stream-select"
                    value={selectedStreamId || ''}
                    onChange={(e) => handleSelectStream(Number(e.target.value))}
                    disabled={engineActive || engineAction}
                  >
                    {streams.map((s) => (
                      <option value={s.id} key={s.id}>
                        {s.applicationName} {s.streamName ? `— ${s.streamName}` : ''} {s.isActive ? '▶ (Playing)' : '⏸ (Paused)'} (Stream {s.id})
                      </option>
                    ))}
                  </select>
                ) : (
                  <span className="summary-value">
                    <em>No audio playback streams detected. Start playback in a media player or browser.</em>
                  </span>
                )}
              </div>
            </div>

            {engineActive && engineSession && (
              <div className="engine-topology-card">
                <div className="topology-header">
                  <h4>Engine Audio Graph</h4>
                </div>
                <div className="topology-nodes">
                  <div className="topology-node">
                    <span className="node-kind">Ingress Sink</span>
                    <span className="node-id">{engineSession.ingressSinkId}</span>
                    <span className={`status ${engineSession.ingressStatus || 'active'}`}>{engineSession.ingressStatus || 'active'}</span>
                  </div>
                  <div className="topology-arrow">↓</div>
                  <div className="topology-node">
                    <span className="node-kind">Tap Source</span>
                    <span className="node-id">{engineSession.tapSourceId}</span>
                    <span className={`status ${engineSession.tapStatus || 'active'}`}>{engineSession.tapStatus || 'active'}</span>
                  </div>
                </div>

                <div className="branches-section">
                  <h4>Active Branches ({engineSession.activeBranchCount || 0} active)</h4>
                  <div className="branch-list">
                    {(engineSession.branches || []).map((branch) => (
                      <div className={`branch-item branch-${branch.state}`} key={branch.branchSinkId}>
                        <span className="branch-indicator">
                          {branch.state === 'active' ? '✓' : branch.state === 'disconnected' ? '⚠' : '✗'}
                        </span>
                        <div className="branch-info">
                          <span className="branch-name">{branch.sinkName}</span>
                          <span className="branch-target">→ {branch.branchSinkId} → {branch.sinkId}</span>
                          {branch.error && <span className="branch-error">{branch.error}</span>}
                        </div>
                        <span className={`status ${branch.state}`}>{branch.state}</span>
                      </div>
                    ))}
                  </div>
                </div>
              </div>
            )}

            <div className="stream-actions">
              <button
                className="button primary"
                onClick={startEngineSession}
                disabled={
                  !selectedStream ||
                  selectedEngineSinkIds.size === 0 ||
                  engineActive ||
                  engineAction
                }
              >
                {engineAction && !engineActive ? 'Starting Engine…' : 'Start Engine'}
              </button>
              <button
                className="button secondary"
                onClick={stopEngineSession}
                disabled={!engineActive || engineAction}
              >
                {engineAction && engineActive ? 'Stopping Engine…' : 'Stop Engine'}
              </button>
            </div>
          </div>
        </section>
      )}
    </main>
  );
}

export default App;
