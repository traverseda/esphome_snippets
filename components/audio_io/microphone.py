import esphome.codegen as cg
from esphome.components import audio, microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID

from . import CONF_AUDIO_IO_ID, AudioIO, audio_io_ns

DEPENDENCIES = ["audio_io"]

AudioIOMicrophone = audio_io_ns.class_("AudioIOMicrophone", microphone.Microphone, cg.Component)


def _set_stream_limits(config):
    # Three 32-bit channels in a fixed order: 0 = mic1, 1 = mic2, 2 = AEC reference.
    # Consumers pick channels and bit depth through a MicrophoneSource.
    audio.set_stream_limits(
        min_bits_per_sample=32,
        max_bits_per_sample=32,
        min_channels=3,
        max_channels=3,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioIOMicrophone),
            cv.GenerateID(CONF_AUDIO_IO_ID): cv.use_id(AudioIO),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    hub = await cg.get_variable(config[CONF_AUDIO_IO_ID])
    cg.add(var.set_parent(hub))
