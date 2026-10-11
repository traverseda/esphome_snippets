import esphome.codegen as cg
from esphome.components import audio, microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID

from . import CONF_VOICE_FRONTEND_ID, VoiceFrontend, voice_frontend_ns

DEPENDENCIES = ["voice_frontend"]

VoiceFrontendMicrophone = voice_frontend_ns.class_(
    "VoiceFrontendMicrophone", microphone.Microphone, cg.Component
)


def _set_stream_limits(config):
    # Two 16-bit channels: 0 = processed output (what wake word and STT use),
    # 1 = the debug tap (whichever stage `tap` selects), for A/B listening.
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=2,
        max_channels=2,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(VoiceFrontendMicrophone),
            cv.GenerateID(CONF_VOICE_FRONTEND_ID): cv.use_id(VoiceFrontend),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    parent = await cg.get_variable(config[CONF_VOICE_FRONTEND_ID])
    cg.add(var.set_parent(parent))
