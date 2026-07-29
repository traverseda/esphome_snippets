# esphome_snippets

Reusable [ESPHome](https://esphome.io/) packages, pulled straight from GitHub.
Copy a block below into a new device file, change the name, `esphome run` it.

Credentials stay in your file — a config fetched from GitHub can't resolve
`!secret` against your machine's `secrets.yaml`. What each package does, and
which ids it expects you to leave alone, is in the header comment of
[fairy-lights.yaml](fairy-lights.yaml) and [smart-speak.yaml](smart-speak.yaml).

## `secrets.yaml`

```yaml
wifi_ssid: "your-network"
wifi_password: "..."
ap_password: "..."               # fallback hotspot, min 8 chars
ota_password: "..."              # any string; keep it stable or OTA breaks
api_key_fairy_lights: "..."      # esphome generates these; one per device
api_key_smart_speak: "..."
```

## Fairy lights

```yaml
esphome:
  name: fairy-lights-living-room
  friendly_name: Fairy Lights Living Room

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  fast_connect: true
  power_save_mode: none
  # 8.5dB is the minimum; it fixed connecting but tanks throughput/latency.
  # Walk this down from 15dB only if connection problems return.
  output_power: 15dB
  ap:
    ssid: "Fairy-Lights Fallback Hotspot"
    password: !secret ap_password

captive_portal:
web_server:
  port: 80

api:
  encryption:
    key: !secret api_key_fairy_lights

ota:
  - platform: esphome
    password: !secret ota_password

packages:
  fairy: github://traverseda/esphome_snippets/fairy-lights.yaml@main
```

To repin or rename, add before the `packages:` block:

```yaml
substitutions:
  light_name: "Window Lights"
  a_in1_pin: GPIO6
  a_in2_pin: GPIO7
  b_in1_pin: GPIO8
  b_in2_pin: GPIO9
  pwm_frequency: "4882Hz"
```

## Smart speaker

Needs ESPHome 2026.7.0+ and `framework: esp-idf`. The audio path runs on the
[esphome-audio-stack](https://github.com/n-IA-hane/esphome-audio-stack) external
component instead of ESPHome's `i2s_audio`, so that acoustic echo cancellation
can use the board's hardware echo reference — the Waveshare schematic feeds the
ES8311's output back into ES7210 channel 3 through a divider, and ESPHome's own
ES7210 driver cannot read it because it does not do TDM. Without this the device
goes deaf whenever it is playing anything.

That component is young and has one maintainer, so pin a tag rather than
tracking `main` (see "To stop tracking `main`" below). `v1-pre-aec` is the last
release of this package before the migration if you need to go back.

It also does not include the BLE beacon proxy, and cannot — see
[Known limitation: a TV in the room](#known-limitation-a-tv-in-the-room) and
the comment at the top of `smart-speak.yaml`.

```yaml
esphome:
  name: smart-speak-living-room
  friendly_name: Smart Speak Living Room

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Smart-Speak-Living-Room"
    password: !secret ap_password

captive_portal:

api:
  encryption:
    key: !secret api_key_smart_speak

ota:
  - platform: esphome
    password: !secret ota_password

packages:
  speaker: github://traverseda/esphome_snippets/smart-speak.yaml@main
```

Don't add `id:` to your `wifi:` or `api:` — the package sets `wifi_id` and
`api_id` and its LED scripts read them.

## Known limitation: a TV in the room

**Symptom.** Say the wake word while a TV is playing dialogue and the command
never ends. The device keeps streaming until the TV happens to pause, or until
the pipeline gives up after 15 s.

**This is not a microphone problem, and nothing in this package fixes it.** It
is end-of-speech detection, and it happens in Home Assistant, not on the device.
`homeassistant/components/assist_pipeline/vad.py`, `VoiceCommandSegmenter`:

| field | default | meaning |
|---|---|---|
| `speech_seconds` | 0.3 | speech needed before a command starts |
| `command_seconds` | 1.0 | minimum command length |
| `silence_seconds` | **0.7** | **continuous non-speech needed to end the command** |
| `timeout_seconds` | 15.0 | hard cap, logs "VAD end of speech detection timed out" |
| `in_command_speech_threshold` | 0.5 | probability above which a chunk counts as speech |

TV dialogue almost never leaves 0.7 s of continuous non-speech, so nothing ends
the command and it runs to the 15 s timeout.

### What to try

1. **Set "Finished speaking detection" to `aggressive`** on the satellite device
   in HA. It is the only exposed control and it only changes `silence_seconds`:
   `aggressive` 0.25 s, `default` 0.7 s, `relaxed` 1.25 s. A 0.25 s gap appears
   between ordinary words, so this is the one setting that acts on the actual
   mechanism. Cost: it will also cut *you* off if you pause mid-sentence. No
   firmware change, reversible in seconds.
2. **Move the speaker away from the TV.** Still the highest-leverage fix.

`timeout_seconds` and `in_command_speech_threshold` are not exposed in the UI.

### What does not work, and why

Checked against ESPHome 2026.7.3, esp-sr 2.4.7, esphome-audio-stack v2026.7.0,
HA 2026.7.4, on 2026-07-29. Recorded so nobody re-treads these.

- **AEC.** Cancels only what the device itself plays, because that is the only
  signal it has a reference for. A TV is uncorrelated. Works beautifully for its
  actual job (~20 dB measured) and is irrelevant here.
- **Dual-mic BSS (`se_enabled`).** Separates sources spatially, which needs the
  TV and the talker at different angles. Fails when the device sits next to the
  TV. Worse, `fixed_output_channel` is false, so the AFE chooses which separated
  stream to emit and nothing stops it choosing the TV.
- **Wake-word-anchored channel locking.** The correct mechanism, and ESP-SR
  implements it — `wakenet_state_t` has `WAKENET_CHANNEL_VERIFIED`, and
  `det_mode_t` has `DET_MODE_2CH_*` / `DET_MODE_3CH_*` to run detection across
  raw channels and pin the output to the one the wake word was in. Unreachable
  three times over: `cfg->wakenet_init = false` is hardcoded in the component's
  `esp_afe.cpp`; `micro_wake_word` runs *downstream* of the AFE on the already
  mixed mono stream, so it cannot inform a decision made upstream of it; and the
  partition table has no `srmodels` partition to hold WakeNet models, so adding
  one means repartitioning and a **serial** flash of every device.
- **Switching to WakeNet wholesale.** 75 stock models ship with esp-sr. The
  catalogue has `wn9_jarvis_tts` ("Jarvis") but no "Hey Jarvis", so you would
  trade a two-word wake phrase for a one-word one — more false fires, which is
  worse in a noisy room, not better.
- **Converting the microWakeWord models to WakeNet9.** No path. esp-sr's `tool/`
  directory has MultiNet helpers only, nothing for WakeNet. Models are opaque
  binaries (`wn9_data`, `wn9_index`) with no published format. The architectures
  differ (dilated convolution vs streaming TFLite-micro) and so do the feature
  frontends, so weights are not transferable even in principle. Espressif offers
  no public trainer; third parties charge four figures per model.
- **Direction of arrival.** `esp_doa_create` / `esp_doa_process` exist and link
  from `libesp_audio_processor.a`, returning 0–180°. But **nothing consumes an
  angle.** `mase_create(fs, frame_size, array_type, mic_distance, operating_mode,
  filter_strength)` has no steering parameter despite offering a
  `WAKE_UP_ENHANCEMENT_MODE`. A bearing is a number you can publish as a sensor
  and nothing more.
- **Hand-rolled null-steering at the TV.** Physically possible, poor payoff on
  this board: ~65 mm spacing gives ~190 µs of total inter-mic delay, about three
  samples at 16 kHz, so steering needs fractional-delay interpolation; a two-mic
  line array nulls a *cone*, so it also nulls whatever mirrors the TV about the
  mic axis; only the direct path is nulled while reverberation arrives off-axis,
  capping real-world rejection near 6–10 dB; and a TV a few tens of centimetres
  away is in the near field, breaking the plane-wave assumption the method rests
  on.
- **A push-to-talk button.** Works, and defeats the point of a voice assistant.

### What would change the answer

- **TV audio available as a signal.** AEC gets ~20 dB because it holds a
  sample-exact copy of the interference, not because it knows where it came
  from. If TV audio ever routes through something tappable, feeding it as a
  second AEC reference beats every spatial approach here — at the cost of
  patching the AFE for a network-sourced reference and solving clock drift and
  latency alignment.
- **`esphome-audio-stack` exposing per-channel BSS outputs.** Then run wake-word
  detection on each separated channel and lock the output to whichever fires —
  a hand-built `WAKENET_CHANNEL_VERIFIED`. Costs a second `micro_wake_word`
  instance, which is not free on a device already at ~48% internal RAM.
- **Different hardware.** More array elements, wider spacing, or a dedicated
  front end such as the XMOS XVF3800, which does AEC, de-reverberation and DoA
  in silicon.

## Tweaking without forking

Your substitutions beat the package's, and `!extend` / `!remove` reach into
entities the package defined:

```yaml
number:
  - id: !extend max_duty_number
    initial_value: 70

light:
  - id: !extend fairy
    effects: !remove
```

Ids are global, so a snippet goes in once per device.

To stop tracking `main`, pin a ref and control caching with the long form:

```yaml
packages:
  fairy:
    url: https://github.com/traverseda/esphome_snippets
    files: [fairy-lights.yaml]
    ref: v1              # tag, branch, or commit
    refresh: 1d          # re-fetch interval; 0s = every build
```
