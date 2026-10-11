"""Raw audio I/O for the Waveshare ESP32-S3-AUDIO-Board: I2S TDM, ES7210, ES8311.

Phase 0 of docs/rewrite-plan.md. Capture and playback, no processing: the bus
and codecs are ours, so every sample the mics produce is observable before
anything touches it, and playback volume is the DAC's own register.
"""

import esphome.codegen as cg
from esphome.components import i2c
from esphome.components.esp32 import add_idf_component
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_SAMPLE_RATE
from esphome import pins

CODEOWNERS = ["@traverseda"]
DEPENDENCIES = ["esp32", "i2c"]
AUTO_LOAD = ["audio", "ring_buffer"]
MULTI_CONF = False

CONF_AUDIO_IO_ID = "audio_io_id"
CONF_MCLK_PIN = "mclk_pin"
CONF_BCLK_PIN = "bclk_pin"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_DIN_PIN = "din_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_ES8311 = "es8311"
CONF_VOLUME_MAX_DB = "volume_max_db"
CONF_FRAME_MS = "frame_ms"
CONF_ES7210 = "es7210"
CONF_MIC_GAIN_DB = "mic_gain_db"
CONF_REF_ADC_CHANNEL = "ref_adc_channel"
CONF_REF_GAIN_DB = "ref_gain_db"
CONF_SLOTS = "slots"
CONF_MIC1 = "mic1"
CONF_MIC2 = "mic2"
CONF_REF = "ref"
CONF_STATS_INTERVAL = "stats_interval"
CONF_MIC_HPF = "mic_hpf"

# Same esp_codec_dev the external stack used, so ES7210 init is byte-identical.
ESP_CODEC_DEV_REF = "1.5.10"

audio_io_ns = cg.esphome_ns.namespace("audio_io")
AudioIO = audio_io_ns.class_("AudioIO", cg.Component)

_GAIN = cv.float_range(min=0.0, max=37.5)
_SLOT = cv.int_range(min=0, max=3)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AudioIO),
        cv.GenerateID(i2c.CONF_I2C_ID): cv.use_id(i2c.I2CBus),
        cv.Required(CONF_MCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
        # Playback. Without it the bus is capture only and no speaker can attach.
        cv.Optional(CONF_DOUT_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.one_of(16000, int=True),
        # One read = one hand-off downstream. 20 ms matches the RTP packetisation.
        cv.Optional(CONF_FRAME_MS, default=20): cv.int_range(min=10, max=40),
        cv.Optional(CONF_ES7210, default={}): cv.Schema(
            {
                cv.Optional(CONF_ADDRESS, default=0x40): cv.i2c_address,
                # Analog PGA for the mics. The external stack ran 30 dB.
                cv.Optional(CONF_MIC_GAIN_DB, default=30.0): _GAIN,
                # The AEC reference is the DAC output through a divider.
                # This is the physical ADC channel (MIC1=0..MIC4=3), NOT the TDM slot.
                cv.Optional(CONF_REF_ADC_CHANNEL, default=2): cv.int_range(min=0, max=3),
                # Measured with playback: at 0 dB the reference sits ~25 dB under
                # the echo and AEC removes ~5 dB; 12-24 dB gives ~18 dB.
                cv.Optional(CONF_REF_GAIN_DB, default=18.0): _GAIN,
                # Hardware high-pass on the mics; see AudioIO::set_mic_hpf. 2 was
                # best by ear on this board (3-5 worse, 0 unusable); 10 =
                # esp_codec_dev's default ("off").
                cv.Optional(CONF_MIC_HPF, default=2): cv.int_range(min=1, max=13),
            }
        ),
        cv.Optional(CONF_ES8311, default={}): cv.Schema(
            {
                cv.Optional(CONF_ADDRESS, default=0x18): cv.i2c_address,
                # DAC volume at 100%. The pre-AEC firmware peaked at +6.5 dB on this
                # register (volume_max 0.8 -> 0xCC), with the NS4150B clean there.
                cv.Optional(CONF_VOLUME_MAX_DB, default=6.5): cv.float_range(min=-20.0, max=32.0),
            }
        ),
        # Measured on this board: the ES7210 lays the frame out MIC1, MIC3(ref), MIC2, MIC4.
        cv.Optional(CONF_SLOTS, default={}): cv.Schema(
            {
                cv.Optional(CONF_MIC1, default=0): _SLOT,
                cv.Optional(CONF_REF, default=1): _SLOT,
                cv.Optional(CONF_MIC2, default=2): _SLOT,
            }
        ),
        # How often the capture task logs per-channel RMS/peak/DC. 0 disables.
        cv.Optional(CONF_STATS_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    add_idf_component(name="espressif/esp_codec_dev", ref=ESP_CODEC_DEV_REF)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[i2c.CONF_I2C_ID])
    cg.add(var.set_i2c_bus(bus))

    cg.add(
        var.set_pins(
            config[CONF_MCLK_PIN],
            config[CONF_BCLK_PIN],
            config[CONF_LRCLK_PIN],
            config[CONF_DIN_PIN],
            config.get(CONF_DOUT_PIN, -1),
        )
    )
    dac = config[CONF_ES8311]
    cg.add(var.set_es8311(dac[CONF_ADDRESS], dac[CONF_VOLUME_MAX_DB]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_frame_ms(config[CONF_FRAME_MS]))

    es = config[CONF_ES7210]
    cg.add(var.set_es7210(es[CONF_ADDRESS], es[CONF_MIC_GAIN_DB], es[CONF_REF_ADC_CHANNEL], es[CONF_REF_GAIN_DB]))
    cg.add(var.set_mic_hpf_coefficient(es[CONF_MIC_HPF]))

    slots = config[CONF_SLOTS]
    cg.add(var.set_slots(slots[CONF_MIC1], slots[CONF_MIC2], slots[CONF_REF]))
    cg.add(var.set_stats_interval(config[CONF_STATS_INTERVAL]))
