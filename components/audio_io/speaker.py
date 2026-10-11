import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)

from . import CONF_AUDIO_IO_ID, AudioIO, audio_io_ns

DEPENDENCIES = ["audio_io"]

AudioIOSpeaker = audio_io_ns.class_(
    "AudioIOSpeaker", speaker.Speaker, cg.Component, cg.Parented.template(AudioIO)
)

def _set_stream_limits(config):
    # 16-bit mono at the bus rate: one DAC, one amplifier, one speaker.
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioIOSpeaker),
            cv.GenerateID(CONF_AUDIO_IO_ID): cv.use_id(AudioIO),
            cv.Optional(CONF_BUFFER_DURATION, default="500ms"): cv.positive_time_period_milliseconds,
            # Fixed by the bus, but stated so the mixer/resampler can inherit them
            # at config time.
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.one_of(16000, int=True),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, int=True),
            cv.Optional(CONF_NUM_CHANNELS, default=1): cv.one_of(1, int=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    await cg.register_parented(var, config[CONF_AUDIO_IO_ID])
    cg.add(var.set_buffer_duration(config[CONF_BUFFER_DURATION]))
