const ALL_SPEAKERS_SINK = 'all_speakers';

class StreamRoutingService {
  constructor(audioBackend, aggregateSinkName = ALL_SPEAKERS_SINK) {
    this.audioBackend = audioBackend;
    this.aggregateSinkName = aggregateSinkName;
    this.routedStreams = new Map();
  }

  async listApplicationStreams() {
    const [aggregateSink, streams] = await Promise.all([
      this.audioBackend.findSinkByName(this.aggregateSinkName),
      this.audioBackend.listApplicationStreams()
    ]);
    const streamsById = new Map(streams.map((stream) => [stream.id, stream]));

    this.reconcileRoutedStreams(streamsById, aggregateSink);

    return {
      aggregate: aggregateSink
        ? { available: true, id: aggregateSink.id, name: aggregateSink.name }
        : { available: false, id: this.aggregateSinkName, name: 'All Speakers' },
      streams: streams.map((stream) => {
        const routedStream = this.routedStreams.get(stream.id);

        return {
          ...stream,
          routedBySpeakerFlow: Boolean(routedStream),
          originalSinkId: routedStream?.originalSinkId || null,
          originalSinkName: routedStream?.originalSinkName || null
        };
      })
    };
  }

  async routeToAllSpeakers(streamId) {
    const [aggregateSink, streams] = await Promise.all([
      this.audioBackend.findSinkByName(this.aggregateSinkName),
      this.audioBackend.listApplicationStreams()
    ]);
    if (!aggregateSink) {
      throw new Error('The existing all_speakers output is unavailable. No audio routing was changed.');
    }

    const stream = streams.find((item) => item.id === Number(streamId));
    if (!stream) {
      throw new Error('The selected application stream is no longer available.');
    }
    if (this.routedStreams.has(stream.id)) {
      throw new Error('SpeakerFlow already routed this stream to All Speakers.');
    }
    if (stream.currentSinkId === aggregateSink.id) {
      throw new Error('This stream already targets All Speakers outside SpeakerFlow; its original output is unknown.');
    }

    await this.audioBackend.moveSinkInput(stream.id, aggregateSink.id);
    await this.verifySink(stream.id, aggregateSink.id, 'All Speakers');
    this.routedStreams.set(stream.id, {
      originalSinkId: stream.currentSinkId,
      originalSinkName: stream.currentSinkName
    });
  }

  async restoreOriginalOutput(streamId) {
    const routedStream = this.routedStreams.get(Number(streamId));
    if (!routedStream) {
      throw new Error('SpeakerFlow has no original-output record for this stream.');
    }

    const originalSink = await this.audioBackend.findSinkByName(routedStream.originalSinkId);
    if (!originalSink) {
      throw new Error(`The original output, ${routedStream.originalSinkName}, is unavailable. The stream was not moved.`);
    }

    await this.verifySink(Number(streamId), this.aggregateSinkName, 'All Speakers');
    await this.audioBackend.moveSinkInput(Number(streamId), originalSink.id);
    await this.verifySink(Number(streamId), originalSink.id, routedStream.originalSinkName);
    this.routedStreams.delete(Number(streamId));
  }

  async moveStreamOutput(streamId, targetSinkId, speakerEngineService) {
    const streams = await this.audioBackend.listApplicationStreams();
    const stream = streams.find((item) => item.id === Number(streamId));
    if (!stream) {
      throw new Error('The selected application stream is no longer available.');
    }

    if (targetSinkId === 'speakerflow_engine') {
      if (speakerEngineService?.session) {
        await this.audioBackend.moveSinkInput(stream.id, speakerEngineService.session.ingressSinkId);
        speakerEngineService.session.streamId = stream.id;
        speakerEngineService.session.streamName = stream.applicationName;
      } else if (speakerEngineService) {
        const allOutputs = await this.audioBackend.listOutputDevicesWithStatus();
        const physicalOutputs = allOutputs.filter(
          (d) => !d.isVirtual && !d.id.startsWith('speakerflow.session.') && d.id !== 'all_speakers'
        );
        if (physicalOutputs.length === 0) {
          throw new Error('No physical audio outputs available to start SpeakerFlow Engine.');
        }
        await speakerEngineService.startSession(stream.id, physicalOutputs.map((o) => o.id));
      }
      return;
    }

    if (targetSinkId === this.aggregateSinkName) {
      if (speakerEngineService?.session?.streamId === stream.id) {
        await speakerEngineService.stopSession({ restore: false }).catch(() => {});
      }
      await this.routeToAllSpeakers(stream.id);
      return;
    }

    // Target is a specific physical or virtual sink
    const targetSink = await this.audioBackend.findSinkByName(targetSinkId);
    if (!targetSink) {
      throw new Error(`Target audio sink "${targetSinkId}" was not found in PipeWire.`);
    }

    if (speakerEngineService?.session?.streamId === stream.id) {
      await speakerEngineService.stopSession({ restore: false }).catch(() => {});
    }

    if (this.routedStreams.has(stream.id)) {
      this.routedStreams.delete(stream.id);
    }

    await this.audioBackend.moveSinkInput(stream.id, targetSink.id);
    await this.verifySink(stream.id, targetSink.id, targetSink.name);
  }

  async verifySink(streamId, expectedSinkId, expectedSinkName) {
    const streams = await this.audioBackend.listApplicationStreams();
    const stream = streams.find((item) => item.id === Number(streamId));

    if (!stream) {
      throw new Error('The application stream disappeared before routing could be verified.');
    }
    if (stream.currentSinkId !== expectedSinkId) {
      throw new Error(`Routing verification failed: the stream is not on ${expectedSinkName}.`);
    }
  }

  reconcileRoutedStreams(streamsById, aggregateSink) {
    for (const [streamId] of this.routedStreams) {
      const stream = streamsById.get(streamId);
      if (!stream || !aggregateSink || stream.currentSinkId !== aggregateSink.id) {
        this.routedStreams.delete(streamId);
      }
    }
  }
}

module.exports = { StreamRoutingService, ALL_SPEAKERS_SINK };
