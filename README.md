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
