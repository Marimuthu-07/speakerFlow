# SpeakerFlow

SpeakerFlow is a Linux desktop audio output manager built with Electron and React for discovering PipeWire output devices and selecting multiple outputs in one place. It monitors PipeWire device, stream, and default-output changes through native PipeWire events.

## Run locally

```bash
npm ci
npm run dev
```

`npm run dev` starts Vite and opens Electron. The audio engine reads real PipeWire sink and stream data directly through its native Node-API addon (`libpipewire-0.3`); it does not create, remove, reroute, or otherwise modify persistent PipeWire configuration.

## Build

```bash
npm run build
```

This writes the renderer bundle to `dist/`. To open the built app, run `npm run start` after building.

## Architecture

- `src/`: React/Vite renderer.
- `electron/main.cjs`: Electron desktop shell and IPC registration.
- `electron/preload.cjs`: minimal renderer API surface.
- `electron/audio/`: platform audio backend abstraction (`LinuxPipeWireAudioBackend`), pipeline adapter, and motion controllers communicating directly with PipeWire via the native addon.
- `native/`: Node-API C addon (`native/pipewire_binding.c`) compiling against `libpipewire-0.3` and `libspa-0.2`.

### Audio Engine Architecture

```text
Electron main process
        ↓
LinuxPipeWireAudioBackend
        ↓
Node-API native addon (`speakerflow_pipewire.node`)
        ↓
libpipewire-0.3 / libspa
        ↓
PipeWire / WirePlumber
```

The native addon interfaces directly with PipeWire to provide:
- `listSinks` & `listSinkInputs` (device and application stream discovery)
- `getDefaultSink` & `setDefaultSink` (default audio output management)
- `moveSinkInput` (direct stream routing)
- `setNodeVolume` & `setNodeMute` (hardware & stream volume/mute control)
- `loadModule` & `unloadModule` (in-process loopback pipeline creation)
- Native PipeWire event callbacks (real-time device, stream, and default sink updates)

The audio engine operates entirely in-process via `libpipewire-0.3` without depending on external audio CLI utilities (such as `pactl`, `pw-cli`, `pw-dump`, `pw-metadata`, or `pw-loopback`).

`Connect All` intentionally selects every detected output in the user interface. It does not alter the system audio graph.

`Route to All Speakers` moves a selected application stream to the already-existing `all_speakers` PipeWire sink using native stream routing (`moveSinkInput`). SpeakerFlow records the original sink in memory and can restore it with `Restore Original Output`. It does not modify, unload, or recreate the existing `all_speakers` configuration, and it does not make the group the system default. The aggregate's configured members remain owned by PipeWire; individual UI output selection does not change them.

## Multi-Speaker Engine prototype (Phase 4)

The Multi-Speaker Engine prototype provides temporary 1-to-N fan-out routing directly on PipeWire without modifying WirePlumber, `all_speakers`, or the system default sink.

### Topology

```text
Selected application stream
        ↓
SpeakerFlow temporary ingress (`Audio/Sink`)
        ↓
SpeakerFlow temporary tap (`Audio/Source`)
        ↓
 ┌──────────────┬──────────────┬──────────────┐
 ↓              ↓              ↓
Branch 1       Branch 2       Branch 3 (`libpipewire-module-loopback`)
 ↓              ↓              ↓
Physical 1     Physical 2     Physical 3
```

- Exactly one temporary ingress virtual sink and one temporary tap virtual source per session.
- Independent temporary loopback branches (`libpipewire-module-loopback`) loaded per selected physical output sink.
- Per-branch error and disconnect isolation (hot-unplugging one speaker disables only that branch while keeping remaining branches, ingress, and tap alive).
- All temporary PipeWire modules and nodes are cleanly destroyed when the session stops, and the application stream is safely restored to its original output.

## Spatial Wave Engine (Phase 6)

The Spatial Wave Engine expands the wave controller into a configurable spatial movement engine supporting four distinct movement patterns with independent runtime branch gain crossfading:

```text
WaveEngineController (30 Hz Coalesced Scheduler, Telemetry, Native Volume Dispatcher)
            ↓
WavePattern Engine (`electron/audio/wavePatterns.cjs`)
    ├── CircularPattern ($A \to B \to C \to D \to A$)
    ├── PingPongPattern ($A \to B \to C \to D \to C \to B \to A$)
    ├── PulsePattern ($A+B \to B+C \to C+D \to D+A$)
    └── ChasePattern ($A_{100\%} \to B_{60\%} \to C_{25\%} \to D_{5\%}$)
```

### Movement Patterns

1. **Circular**:
   - Smooth continuous rotating crossfade around $N$ ordered speakers ($S_1 \to S_2 \to \dots \to S_N \to S_1$).
   - Supports `Forward` and `Reverse` rotational direction.

2. **Ping-Pong**:
   - Bidirectional wave bouncing continuously between boundary speakers ($S_1 \to S_2 \dots \to S_N \to S_{N-1} \dots \to S_1$).
   - Cycle length is $2(N - 1)$. Endpoint direction reverses naturally without abrupt jumps.
   - UI direction control selects the initial travel direction (`Forward`: $S_1 \to S_N$, `Reverse`: $S_N \to S_1$).

3. **Pulse**:
   - Moving multi-speaker region where multiple adjacent speakers participate in a traveling window of configurable width $W \in [1, N]$.
   - As the window advances, the trailing speaker fades out while the leading speaker fades in, keeping interior speakers at full gain.
   - Automatically clamps to the active speaker count on dynamic hot-unplug events.

4. **Chase**:
   - Focused leader speaker with trailing followers decaying via configurable falloff ($f \in [0.0, 1.0]$).
   - $0\%$ = Broad/even chase tail ($\sim 85\%, 72\%, 61\%$).
   - $50\%$ = Balanced trailing decay ($\sim 55\%, 30\%, 16\%$).
   - $100\%$ = Sharp focused leader ($\sim 15\%, 2\%, 0\%$).

### Centralized Intensity & Smoothness

- The scheduler applies intensity centrally across all patterns:
  $$\text{finalGain} = (1 - \text{intensity}) \times 1.0 + \text{intensity} \times \text{patternGain}$$
- At $0\%$: all speakers receive equal gain ($100\%$).
- At $100\%$: maximum pattern contrast and isolation.
- Continuous session clock and non-blocking in-flight coalescing ensure smooth, click-free audio transitions without queue buildup.
- Real-time PipeWire telemetry and branch gain meters provide authoritative visualization driven by the backend audio clock.

## True Spatial Audio Engine (Phase 7)

Phase 7 introduces 2D coordinate-based sound positioning and room layout visualization on top of the Multi-Speaker Engine:

```text
               Movement Mode Switcher
         ┌────────────────┴────────────────┐
         ↓                                 ↓
Pattern Wave Mode (Phase 6)     Spatial Position Mode (Phase 7)
(`wavePatterns.cjs`)            (`spatialAudio.cjs`)
         │                                 │
         └────────────────┬────────────────┘
                          ↓
               Centralized Intensity
                          ↓
               WaveEngineController
             (30 Hz Coalesced Loop)
                          ↓
             Native setNodeVolume
```

### Coordinate System & Acoustic Models

- **Normalized 2D Coordinates**:
  - $x \in [-1, +1]$: $-1 = \text{Left}, 0 = \text{Center}, +1 = \text{Right}$
  - $y \in [-1, +1]$: $-1 = \text{Back}, 0 = \text{Center}, +1 = \text{Front}$
- **Speaker Placement**:
  - Physical speakers receive persistent 2D room coordinates mapped to stable logical sink IDs.
  - Interactive drag-and-drop on the 2D Room Canvas or fine-grained numeric adjustment in the layout table.
- **Sound Source Trajectories**:
  1. **Static**: Direct manual positioning anywhere on the 2D room canvas.
  2. **Orbit**: Continuous orbital movement around center $(0, 0)$ with configurable radius and speed.
  3. **Horizontal Sweep**: Smooth ping-pong oscillation across the left-to-right axis.
  4. **Vertical Sweep**: Smooth ping-pong oscillation across the back-to-front axis.
- **Distance Attenuation & Falloff**:
  $$\text{dist}_i = \sqrt{(sourceX - sx_i)^2 + (sourceY - sy_i)^2}$$
  $$g_{\text{raw}}[i] = \frac{1}{1 + \left(\frac{\text{dist}_i}{d_0(f)}\right)^{1 + 3f}}$$
  Normalized so the closest speaker reaches $1.0$ (safe against division by zero and coordinate overlaps).
- **Spatial Falloff Control**:
  - $\text{Soft } (0\%)$: Broad acoustic spread with gentle ambient fill.
  - $\text{Medium } (50\%)$: Natural balanced distance curve.
  - $\text{Focused } (100\%)$: High focal isolation targeting the closest speaker with rapid drop-off.

## Pavucontrol-style Volume & Routing Architecture (Phase 7.1)

Phase 7.1 adds a multi-tier mixer and routing control layer modeled after `pavucontrol`:

```text
Application Stream Volume (Native setNodeVolume)
                 │
                 ▼
      SpeakerFlow Engine Session Ingress
                 │
                 ▼
        SpeakerFlow Engine Session Tap
                 │
  ┌──────────────┼──────────────┐
  │              │              │
Branch 1       Branch 2       Branch 3
  │              │              │
  ▼              ▼              ▼
targetGain = masterGain × userSpeakerGain × dynamicEngineGain
  │
  ▼
Native setNodeVolume <branch.sinkInputId> <targetPercent>%
```

### Mixer & Gain Rules

- **Independent Multipliers**:
  $$\text{effectiveGain} = \begin{cases} 0, & \text{if } masterMuted \lor speakerMuted \\ masterGain \times userSpeakerGain \times dynamicEngineGain, & \text{otherwise} \end{cases}$$
- **Mute & Unmute**: Muting zeroes the effective branch gain immediately while strictly preserving the slider value. Unmuting restores the exact previous volume.
- **Branch Disconnect / Reconnect**: Disconnecting an output unloads only its loopback module (`libpipewire-module-loopback`) without interrupting playback on remaining branches. Reconnecting an output restores its stable identity, 2D coordinates, user volume, and mute state.
- **Application Output Routing**: Application stream dropdown enables routing streams directly to SpeakerFlow Ingress, `all_speakers`, or physical sinks.
- **Safety Invariant**: Persistent PipeWire configurations, WirePlumber, `all_speakers`, and the system default sink remain untouched.

## Advanced Speaker Motion Engine (Phase 8)

Phase 8 introduces a higher-level motion orchestration layer that makes a single audio stream travel continuously between physical outputs:

```text
                           Engine Mode Switcher
             ┌──────────────────────┼──────────────────────┐
             ↓                      ↓                      ↓
     Motion Engine (Phase 8)   Pattern Wave (Phase 6)  Spatial Position (Phase 7)
     (`motionEngine.cjs`)     (`wavePatterns.cjs`)    (`spatialAudio.cjs`)
             │                      │                      │
             └──────────────────────┼──────────────────────┘
                                    ↓
                         Centralized Intensity
                                    ↓
                          Target Gain Multiplier
              (masterGain × userSpeakerGain × dynamicGain)
                                    ↓
                         WaveEngineController
                        (30 Hz Coalesced Loop)
                                    ↓
             Native setNodeVolume (libpipewire-module-loopback)
```

### Motion Modes

1. **Sequential Travel**:
   - Step-based motion with configurable dwell ratio ($0\% \dots 95\%$) and clean transition/switching across ordered speakers.
2. **Smooth Travel**:
   - Continuous equal-power ($\cos / \sin$, $g_1^2 + g_2^2 = 1.0$), linear, or smoothstep crossfade between neighboring speakers.
3. **Circular Travel**:
   - Closed-loop polygon traversal around ordered speakers with seamless wrap-around at sequence boundaries.
4. **Bounce**:
   - Reversible linear traversal reflecting at endpoint boundaries ($A \to B \to C \to B \to A$).
5. **Shuffle**:
   - Controlled non-repeating pseudo-random traversal ensuring no consecutive duplicate speaker selections (with deterministic seeded PRNG for testing).
6. **Spatial Path**:
   - 2D trajectory generator producing continuous coordinates (Figure-8 $\infty$, Waypoint Polygon, Lissajous Knot, Expanding/Contracting Spiral) that pass directly through the Phase 7 `spatialAudio.cjs` acoustic distance/gain model without duplicating math.

### Key Architectural Invariants

- **Single Authoritative Writer**: All branch volume changes are dispatched exclusively through `WaveEngineController`'s coalesced async dispatcher.
- **Strict Gain Hierarchy**: $\text{effectiveGain} = \text{masterGain} \times \text{userSpeakerGain} \times \text{dynamicEngineGain}$.
- **Safe Dynamic Topology**: $N=0$ and $N=1$ topologies are safely handled across every mode without throwing or freezing. Hot-unplug, disconnect, and reconnect dynamically adjust the motion sequence while strictly preserving user volume, mute state, and spatial coordinates.
- **Zero Invariant Violations**: Leaves `all_speakers`, WirePlumber, persistent PipeWire configurations, and system default sinks completely untouched.

## Linux Support

SpeakerFlow targets Linux desktop systems running PipeWire with WirePlumber.

### Tested

- Arch Linux
- PipeWire
- WirePlumber
- ALSA analog audio output
- Bluetooth audio through BlueZ
- Multiple Bluetooth audio outputs
- Bluetooth + analog/AUX output simultaneously
- Dynamic output connection and disconnection
- Dynamic engine branch recovery
- Multiple simultaneous playback streams

### Linux audio requirements

SpeakerFlow requires:

- PipeWire (>= 0.3) with `libpipewire-0.3`
- WirePlumber
- BlueZ and a working Bluetooth adapter for Bluetooth audio

### Compatibility note

SpeakerFlow has been tested successfully on the configuration listed above.
Linux audio behavior can vary between distributions, PipeWire/WirePlumber
versions, hardware, Bluetooth adapters, and audio devices.

The tested configurations should not be interpreted as a guarantee that every
Linux distribution or audio device is supported.
