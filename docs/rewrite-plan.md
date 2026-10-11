# smart-speak rewrite plan

Status, 2026-10-10: `audio_io` (capture and playback) and `voice_frontend` are built and
running on every speaker, replacing esphome-audio-stack. Measured on alfa:

- Settled by listening: hardware HPF coefficient 2, then +6 dB before AEC and +24 dB after.
  WebRTC NS and AGC both made speech worse, so they default to off.
- AEC reference PGA 18 dB. That takes echo removal from ~5 dB to ~18 dB on a test clip, or
  ~10–12 dB on music.
- Internal heap went from ~28 KB to ~65 KB free.
- The LED ring's playback meter (Sound Reactive, Spectrum) is in C++.

Still to do: volume_manager, ring_state, the package split and the remaining tools/
scripts. The plan below is the original proposal.

## Why

`smart-speak.yaml` is 3,400 lines. It "kind of works" in three places that share one cause:
behaviour lives in YAML lambdas spread across globals, scripts and `on_state` handlers. No
single place owns a decision, and nothing can be measured except by ear.

| Area | What's wrong today | Root cause |
|---|---|---|
| Mic | Raw speech/TV at about −56 dBFS, 60/120 Hz hum 11–15 dB above its surroundings, STT gets ×4 gain of whatever is left. Switching noise suppression off stops the mic stream entirely. | Gain was tuned by trial and error, with no level targets. No high-pass filter. Only the post-processing stream can be observed. Processing is a black box (esp-sr AFE inside a single-maintainer component). |
| Volume | 0–100% maps linearly onto −49..0 dB, so the top half barely changes and the bottom collapses. Max output is 6.5 dB below the old firmware. Ducking is integer dB on mixer inputs. Three sliders (Voice, Sendspin, MA) are kept in sync by mirroring. | No single volume model. The one hardware gain is unreachable, so everything is software attenuation below 0 dB. |
| LEDs | 12 virtual light widgets, 36 template outputs, 24 effect globals. Each widget's `on_state` turns the other 11 off. `control_leds` is a queued script called from ~20 places. | The state is implicit: it's whichever widget happens to be on. Priority logic and rendering are tangled together, and transient flashes call back into the resolver. |

## Principles

1. **Logic in C++ components, wiring in YAML.** `sap_mic` showed the pattern works here. YAML
   declares entities and connects components, and lambdas shrink to one-liners. Anything that
   is a state machine, a DSP stage or a gain calculation is C++, and nothing that makes a
   decision stays in a YAML lambda.
2. **Every stage observable.** Any point in the audio chain can be streamed (SAP), metered
   (sensors) and recorded on the desktop, so we never have to guess what a stage did.
3. **Explicit level targets.** Each stage has a written target range (below), and telemetry
   says whether it's being met.
4. **One owner per decision.** One component resolves LED state, one owns volume, and one owns
   the mic chain.
5. **Measure before and after every change**, using scripts in `tools/` rather than listening.

## Target architecture

```
                 ┌──────────── audio_io (ours: IDF i2s_tdm + esp_codec_dev, ES7210, ES8311) ───┐
 ES7210 TDM ───► │ slot0 MIC1, slot2 MIC2, slot1 REF                                            │
                 └──────┬─────────────────────────────────────────────────────────────────────┘
                        ▼
              voice_frontend (new, ours)
   T0 raw mic1/mic2/ref ─► HPF 120–150 Hz ─► T1 ─► AEC (+BSS?) ─► T2 ─► NS ─► T3
                                                  ─► AGC/limiter to target ─► T4 ─┬─► micro_wake_word
                                                                                  ├─► voice_assistant (STT)
                                                                                  └─► sap_mic (any tap T0–T4)
 volume_manager (new): perceptual curve, per-stream gain, ducking states, codec hardware gain
 ring_state (new): inputs → priority resolver → state → renderer (+ timed overlays)
```

T0–T4 are named taps. Each tap has an RTP stream (one at a time, chosen by a `select`), a
level meter (RMS, peak, noise floor, clip count, 60/120 Hz hum level), and a stable name the
desktop tools use.

### Level targets (initial values, refined in Phase 0)

| Point | Target | Why |
|---|---|---|
| T0 raw, speech at 1 m | peaks −12 to −6 dBFS | Use the ADC's range. Today it is about −56 dBFS. |
| T0 noise floor | ≥ 40 dB below speech | Otherwise the PGA/mic is the bottleneck and no later processing can recover it. |
| Hum (60/120 Hz) after HPF | not visible above the noise floor | Removes the buzz you're hearing. |
| T4 (STT/wake word), speech | RMS −20 ±3 dBFS, peaks ≤ −3 | Normal speech into a decent mic, which is what every STT engine is built for. The limiter prevents clipping. |
| Output, 100% volume | 0 dB at the codec, using ES8311 hardware gain for headroom | Recovers the 6.5 dB lost in the AEC migration. |

## Components

### 0. `audio_io` (replaces `esphome-audio-stack`)
- A thin layer we own: the ESP-IDF `i2s_tdm` driver in full duplex, plus `esp_codec_dev` for
  ES7210/ES8311 init. The IDF and codec libraries do the hard work, so this is a few hundred
  lines rather than the external stack's several thousand.
- One audio task: read a TDM frame, de-interleave into mic1/mic2/ref, hand off to
  `voice_frontend`, and write the TX frame. Buffers in PSRAM, DMA descriptors internal (the
  lessons recorded in today's YAML comments carry over: slot order MIC1, REF, MIC2, MIC4; ref
  at 0 dB PGA; 16 kHz bus).
- Exposes ES7210 PGA and ES8311 DAC/volume registers as runtime setters. That gives hardware
  output headroom back and lets Phase 0 sweep the PGA without reflashing.
- Speaker side: a stock ESPHome `speaker` platform, so the mixer, resampler, media players and
  sendspin above it stay unchanged.
- Why own it rather than fork: we need raw frames, codec registers and the NS-toggle bug fixed.
  Those are three changes deep in someone else's architecture. The external stack's value is
  TDM bring-up knowledge, and we now have that written down.

### 1. `voice_frontend` (mic chain)
- Takes raw de-interleaved frames from `audio_io` and owns the processing order.
- Stages are individually switchable at runtime **without stopping the stream** (today,
  switching NS off kills it). A bypassed stage passes audio through unchanged.
- HPF: 2nd-order biquad, ~0.1% CPU. AGC/limiter: slow gain toward the T4 target, fast peak
  limiter, max gain capped so silence doesn't get pumped up into hiss.
- Keeps esp-sr AFE for AEC/NS (nothing better runs on an S3), but pinned, configured from one
  place, and with taps either side so its effect is measurable. Compare `low_cost` against
  `high_perf`, and BSS on against off, using Phase 1 tooling instead of guessing.
- Exposes `MicrophoneSource`-compatible outputs, so micro_wake_word and voice_assistant stay
  stock ESPHome.

### 2. `sap_mic` (extend)
- `tap:` selector: one stream, retargetable at runtime from HA, plus a "both mics + ref"
  3-channel debug mode (RTP L16/16000/3) for AEC work.
- Already done: follows `va_mic` gain (T4 equivalent until `voice_frontend` exists).

### 3. `volume_manager`
- One model: volume percentage → dB through a published curve. Use cubic, the same as
  PipeWire, PulseAudio and HA. 50% = −18 dB, and every slider feels the same.
- Streams: `voice`, `music` (sendspin), `alerts`. Each has a user level plus a ducking state
  (`none / announcement / voice / timer`) with defined dB and ramp times in one table.
- Master gain: use the ES8311 hardware register for anything above 0 dB, so the software
  stage never needs to boost. Needs a setter in `esp_audio_stack` (see fork, below).
- Publishes actual dB per stream as sensors. MA and the HA sliders write into the model,
  instead of mirroring each other.
- Verification: the AEC reference tap is an electrical copy of the DAC output, so a volume
  sweep can be **measured** (T0 ref level against slider), not judged by ear.

### 4. `ring_state` (LEDs)
- Inputs: wifi/api, improv, init, mute, VA phase, timer ringing/active, music playing.
- `resolve()` is a pure function mapping inputs to one enum state, with the priority order in
  one table (improv > disconnected > init > ringing > VA phases > muted > timers > music >
  idle). Unit-testable on the desktop.
- The renderer applies the state's look. Looks still come from the 12 HA light widgets (kept
  as **config inputs only**, with no `on_state` logic). If you'd rather drop per-state HA
  customisation, a YAML table is much simpler.
- Overlays (mute flash, volume flash) are time-limited layers above the resolved state. They
  expire by themselves and never call back into the resolver.
- Debug: `text_sensor` "LED state" and "LED reason" (which input won), plus a log line on
  every transition.

### 5. Package split
`smart-speak.yaml` becomes a thin top-level file including `hardware.yaml`, `audio.yaml`,
`voice.yaml`, `volume.yaml`, `leds.yaml`, `timers.yaml`, `debug.yaml`. The public package URL
stays the same.

## Debugging toolkit (built first, used by every phase)

On the device:
- Tap streaming (`sap_mic`), tap level meters, a "Debug: audio diagnostics" switch gating all
  telemetry (existing pattern).
- State text sensors: LED state/reason, VA phase, per-stream volume dB, active ducking, front-end
  stage bypass flags.
- Internal heap and largest block sensors (existing). These are the regression alarm; see the
  esp-sr 2.6.0 incident.

On the desktop, in `tools/` (committed, uv/PEP 723 single-file scripts):
- `capture.py <tap> <secs>`: switch the tap over the API, record the RTP stream to WAV.
- `spectrum.py`: level, hum, speech-band and clipping report (what we did ad hoc today).
- `ab.py`: toggle entities over the API, capture and compare each state (today's amp/LED test,
  generalised).
- `volume_sweep.py`: step the volume slider and measure the reference tap; plot dB against %.
- `wakeword_bench.py`: play N positive and negative clips from the desktop at a fixed position,
  count detections from device events. Stops front-end changes silently breaking the wake word.
- STT check: HA `assist_pipeline: debug_recording_dir` captures exactly what STT received.

Reference recordings: a short fixed test set (speech at 1 m, TV, silence, music playing from
the device) captured at T0 and T4 after each phase, kept under `tools/baselines/` with their
metrics, so "better or worse" is a diff.

## Phases

| # | Phase | Exit criteria |
|---|---|---|
| 0 | **Spike `audio_io` and measure.** Minimal `audio_io` (TDM capture only, no AFE, no playback) streaming raw mic1/mic2/ref over `sap_mic`. Gets the raw data, proves the I/O layer, and settles the hum source (board vs room). Calibrate mic sensitivity and PGA (30 dB now, ES7210 goes to ~37.5) with a runtime PGA setter. Measure the RAM budget against the external stack. | Hum source known. Raw T0 levels against the target table. `audio_io` capture stable for 24 h. |
| 1 | **Debug toolkit.** Multi-tap `sap_mic`, tap meters, `tools/` scripts, first baselines. | Any tap recorded and analysed with one command. Baselines committed. |
| 2 | **`voice_frontend`.** HPF, stage order, runtime bypass, AGC/limiter, wake word and STT on T4. AFE mode and BSS chosen by measurement. | T4 meets targets. Hum gone. Wake word bench ≥ baseline. STT debug recordings intelligible. Heap ≥ 20 KB. |
| 3 | **`volume_manager`.** | Measured sweep matches the cubic curve within ±1 dB. 100% output ≥ the old firmware. MA and HA sliders agree. |
| 4 | **`ring_state`.** | Resolver unit tests pass. No double-lit states under rapid changes (today's `diag.py` reproduction). Every transition logged with its reason. |
| 5 | **Package split, README, retire dead globals/scripts.** | Other speakers rebuild from `main` cleanly. |

Phases 3 and 4 don't depend on 2 and can be done in any order after Phase 1.

## Decisions (2026-10-10)

1. **I/O layer: own it.** `audio_io` replaces `esphome-audio-stack`. The external stack stays
   the fallback only if the Phase 0 spike fails. We pin esp-sr ourselves.
2. **Per-state LED looks stay editable in HA.** The 12 stage widgets remain, as plain config
   inputs read by `ring_state`, with no `on_state` logic of their own.
3. **T4 target is normal human speech level, not tuned per STT engine.** STT engines all expect
   ordinary speech, so T4 should sound like a person speaking at a normal level into a decent
   mic, whichever engine is behind HA.
4. **Development and rollout: alfa only.** Nothing from the rewrite goes to the other speakers
   until it's proven on alfa.

## Later

- **Direction of arrival:** the LED ring points at the talker, a dominant-angle debug sensor,
  then beam locking after the wake word. Needs `voice_frontend` first. Limited by two mics
  65 mm apart (front/back ambiguity, coarse resolution); see the README's DoA notes.

## Risks

- **Internal RAM.** About 28 KB is free now. The new DSP is cheap, but the esp-sr AFE mode
  (`high_perf`) and extra taps aren't. Watch the heap sensors every flash.
- **Hardware limits.** If Phase 0 shows the hum or noise floor is on the board's mic bias,
  software can only filter it, not remove it. Better mics or a dedicated front end (XVF3800)
  is the step after that, out of scope here.
- **Upstream churn.** ESPHome's audio APIs move quickly. Keeping our components thin over stock
  `MicrophoneSource`/speaker interfaces limits the blast radius.
- **TDM bring-up.** Owning `audio_io` means redoing the clocking and slot work the external
  stack solved. The slot map, PGA split and buffer placement are already known and documented,
  which is why this is a spike in Phase 0 and not a leap.
