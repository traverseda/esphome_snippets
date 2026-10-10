import esphome.codegen as cg
from esphome.components import microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_PORT

CODEOWNERS = ["@traverseda"]
DEPENDENCIES = ["esp32", "network"]

CONF_SESSION_NAME = "session_name"
CONF_GROUP = "group"
CONF_PACKET_MS = "packet_ms"
CONF_BUFFER_MS = "buffer_ms"
CONF_ENABLED = "enabled"

sap_mic_ns = cg.esphome_ns.namespace("sap_mic")
SapMic = sap_mic_ns.class_("SapMic", cg.Component)


def _multicast_ipv4(value):
    value = cv.ipv4address(value)
    if not 224 <= int(str(value).split(".")[0]) <= 239:
        raise cv.Invalid("group must be an IPv4 multicast address (224.0.0.0-239.255.255.255)")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SapMic),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        # What the stream is called on the receiving end. Defaults to
        # "<friendly_name> mic".
        cv.Optional(CONF_SESSION_NAME): cv.string_strict,
        # RTP destination. Defaults to 239.69.<ip3>.<ip4>, so every device on a
        # /24 gets its own group and receivers only take the streams they asked for.
        cv.Optional(CONF_GROUP): _multicast_ipv4,
        cv.Optional(CONF_PORT, default=46000): cv.port,
        cv.Optional(CONF_PACKET_MS, default=20): cv.int_range(min=4, max=40),
        cv.Optional(CONF_BUFFER_MS, default=500): cv.int_range(min=100, max=2000),
        # Starting state; flip at runtime with set_enabled().
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))
    if CONF_SESSION_NAME in config:
        cg.add(var.set_session_name(config[CONF_SESSION_NAME]))
    if CONF_GROUP in config:
        cg.add(var.set_group(str(config[CONF_GROUP])))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_packet_ms(config[CONF_PACKET_MS]))
    cg.add(var.set_buffer_ms(config[CONF_BUFFER_MS]))
    cg.add(var.set_enabled(config[CONF_ENABLED]))
