const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('speakerFlow', {
  listOutputDevices: () => ipcRenderer.invoke('audio:list-output-devices'),
  listApplicationStreams: () => ipcRenderer.invoke('audio:list-application-streams'),
  routeStreamToAllSpeakers: (streamId) => ipcRenderer.invoke('audio:route-stream-to-all-speakers', streamId),
  restoreStreamOutput: (streamId) => ipcRenderer.invoke('audio:restore-stream-output', streamId),
  moveStreamOutput: (streamId, targetSinkId) => ipcRenderer.invoke('audio:move-stream-output', streamId, targetSinkId),
  setStreamVolume: (streamId, volume) => ipcRenderer.invoke('audio:set-stream-volume', streamId, volume),
  setStreamMute: (streamId, muted) => ipcRenderer.invoke('audio:set-stream-mute', streamId, muted),
  setSinkVolume: (sinkName, volume) => ipcRenderer.invoke('audio:set-sink-volume', sinkName, volume),
  setSinkMute: (sinkName, muted) => ipcRenderer.invoke('audio:set-sink-mute', sinkName, muted),
  getSpeakerEngineStatus: () => ipcRenderer.invoke('audio:get-speaker-engine-status'),
  startSpeakerEngineSession: (streamId, sinkIds) => ipcRenderer.invoke('audio:start-speaker-engine-session', streamId, sinkIds),
  stopSpeakerEngineSession: () => ipcRenderer.invoke('audio:stop-speaker-engine-session'),
  disconnectBranch: (sinkId) => ipcRenderer.invoke('audio:disconnect-branch', sinkId),
  reconnectBranch: (sinkId) => ipcRenderer.invoke('audio:reconnect-branch', sinkId),
  setMasterVolume: (volume) => ipcRenderer.invoke('audio:set-master-volume', volume),
  setMasterMute: (muted) => ipcRenderer.invoke('audio:set-master-mute', muted),
  setSpeakerVolume: (sinkId, volume) => ipcRenderer.invoke('audio:set-speaker-volume', sinkId, volume),
  setSpeakerMute: (sinkId, muted) => ipcRenderer.invoke('audio:set-speaker-mute', sinkId, muted),
  getWaveStatus: () => ipcRenderer.invoke('audio:get-wave-status'),
  setWaveConfig: (config) => ipcRenderer.invoke('audio:set-wave-config', config),
  setMotionConfig: (config) => ipcRenderer.invoke('audio:set-motion-config', config)
});
