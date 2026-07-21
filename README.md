# esphome_snippets

Reusable [ESPHome](https://esphome.io/) packages, pulled straight from GitHub.
Copy a block below into a new device file, change the name, `esphome run` it.

Each snippet carries the hardware and behavior; your file carries the name and
the credentials. A remote package can't resolve `!secret` — the compiling
machine's `secrets.yaml` isn't visible to a config fetched from GitHub — so
credentials always stay in your file.

## `secrets.yaml`

```yaml
wifi_ssid: "your-network"
wifi_password: "..."
ap_password: "..."               # fallback hotspot, min 8 chars
ota_password: "..."              # any string; keep it stable or OTA breaks
api_key_fairy_lights: "..."      # esphome generates these; one per device
api_key_smart_speak: "..."
```

---

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

ESP32-C3 + DRV-style H-bridge on GPIO6–9, two strands of dual-polarity fairy
lights. One light entity, two effects (Crossfade, Fireflies), three config
numbers. Brightness and balance are decoupled — total duty depends on the
slider alone, so effects shift light between strands without pulsing the room.
Math is in the header comment of [fairy-lights.yaml](fairy-lights.yaml).

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

- **Max Strand Duty** (65%) is a thermal limit, not a preference — above 50%
  duty a strand exceeds its AC-supply design power. Thermal-test before raising.
- **Min Power** (3.6%) is the dropout floor; **Gamma** (2.2) shapes the slider.
- The package sets `framework: type: esp-idf`. Arduino has no `phase_angle` on
  `ledc`, which the whole scheme depends on.

---

## Smart speaker

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

Waveshare ESP32-S3 audio board as a Home Assistant voice satellite: ES7210 mic,
ES8311 DAC behind a TCA9555-gated amp, 7-LED WS2812 status ring, three hardware
keys. microWakeWord (`okay_nabu`, `hey_jarvis`, `stop` for barge-in), timers,
alarm clock, and music that ducks under announcements.

- Don't add `id:` to your `wifi:` or `api:` — the package sets `wifi_id` and
  `api_id`, and its LED scripts read them. Your keys merge into those blocks.
- Your `encryption:` merges into the package's `api:`, which also defines
  actions: `set_led_color`, `start_va`, `stop_va`, `set_alarm_time`,
  `set_time_zone`.
- The **Diag: disable microphone** boot handler is inverted: at its default OFF
  it takes the `else` branch and stops the mic, so the mic is dead until you
  switch "disable microphone" *on*.
- `vad: probability_cutoff: 0.03` is very low — expect false wakes in a noisy
  room.
- Pulls a third-party `es8311` fork via `external_components` at build time.

---

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

Ids are global, so a snippet goes in once per device. Two fairy-light strands on
one ESP means forking the file, not including it twice.

To stop tracking `main`, pin a ref and control caching with the long form:

```yaml
packages:
  fairy:
    url: https://github.com/traverseda/esphome_snippets
    files: [fairy-lights.yaml]
    ref: v1              # tag, branch, or commit
    refresh: 1d          # re-fetch interval; 0s = every build
```
