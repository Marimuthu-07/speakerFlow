const { createAudioBackend } = require('../electron/audio/createAudioBackend.cjs');
const { StreamRoutingService } = require('../electron/audio/streamRoutingService.cjs');
const { SpeakerEngineService } = require('../electron/audio/speakerEngineService.cjs');

async function test() {
  const backend = createAudioBackend();
  const streamService = new StreamRoutingService(backend);
  const engineService = new SpeakerEngineService(backend);

  const streamsRes = await streamService.listApplicationStreams();
  console.log('Detected streams:', streamsRes.streams);

  const devicesRes = await backend.listOutputDevicesWithStatus();
  const physicalOutputs = devicesRes.filter(
    (d) => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers'
  );
  console.log('Physical outputs:', physicalOutputs.map(d => ({ id: d.id, name: d.name })));

  const strawberry = streamsRes.streams.find(s => s.applicationName === 'Strawberry') || streamsRes.streams[0];
  if (!strawberry) {
    console.log('No stream found!');
    return;
  }

  console.log('Starting session with stream:', strawberry.id, strawberry.applicationName);
  try {
    await engineService.startSession(strawberry.id, physicalOutputs.map(o => o.id));
    console.log('startSession SUCCEEDED!');
    const status = await engineService.getStatus();
    console.log('Engine status after start:', JSON.stringify(status, null, 2));

    console.log('Testing Master Volume to 50%...');
    await engineService.setMasterVolume(50);
    const status50 = await engineService.getStatus();
    console.log('Engine status after master 50%:', JSON.stringify(status50.session?.wave?.branchGains, null, 2));

    console.log('Testing Master Mute...');
    await engineService.setMasterMute(true);
    const statusMute = await engineService.getStatus();
    console.log('Engine status after master mute:', JSON.stringify(statusMute.session?.wave?.branchGains, null, 2));

    console.log('Stopping session...');
    await engineService.stopSession();
    console.log('stopSession SUCCEEDED!');
  } catch (err) {
    console.error('ERROR during session:', err);
    await engineService.stopSession().catch(() => {});
  }
}

test().catch(console.error);
