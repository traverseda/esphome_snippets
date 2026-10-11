"""Wake-word arbitration between speakers on the same LAN.

When several speakers hear the wake word, Home Assistant keeps whichever
pipeline reaches it first - often not the speaker nearest the talker, which
then has to strain to hear the command. Instead, each speaker that detects the
wake word multicasts how well it heard it (voice_frontend's speech SNR), waits a
short window for the others' claims, and only the best one runs on_win. The
rest run on_lose and stay quiet.
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT, CONF_TRIGGER_ID

from esphome.components.voice_frontend import CONF_VOICE_FRONTEND_ID, VoiceFrontend

CODEOWNERS = ["@traverseda"]
DEPENDENCIES = ["esp32", "network", "voice_frontend"]

CONF_GROUP = "group"
CONF_WINDOW = "window"
CONF_ENABLED = "enabled"
CONF_ON_WIN = "on_win"
CONF_ON_LOSE = "on_lose"

wake_arbiter_ns = cg.esphome_ns.namespace("wake_arbiter")
WakeArbiter = wake_arbiter_ns.class_("WakeArbiter", cg.Component)
WinTrigger = wake_arbiter_ns.class_("WinTrigger", automation.Trigger.template(cg.std_string))
LoseTrigger = wake_arbiter_ns.class_("LoseTrigger", automation.Trigger.template(cg.std_string))

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WakeArbiter),
        cv.GenerateID(CONF_VOICE_FRONTEND_ID): cv.use_id(VoiceFrontend),
        cv.Optional(CONF_GROUP, default="239.255.69.69"): cv.ipv4address,
        cv.Optional(CONF_PORT, default=46969): cv.port,
        # How long to collect other speakers' claims. Detection of one
        # utterance on several speakers lands within a few tens of ms of each
        # other; this also covers WiFi multicast delay and the resends.
        cv.Optional(CONF_WINDOW, default="200ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=50), max=cv.TimePeriod(milliseconds=1000)),
        ),
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_ON_WIN): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(WinTrigger)}
        ),
        cv.Optional(CONF_ON_LOSE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LoseTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    vfe = await cg.get_variable(config[CONF_VOICE_FRONTEND_ID])
    cg.add(var.set_voice_frontend(vfe))
    cg.add(var.set_group(str(config[CONF_GROUP]), config[CONF_PORT]))
    cg.add(var.set_window_ms(config[CONF_WINDOW]))
    cg.add(var.set_enabled(config[CONF_ENABLED]))
    for conf in config.get(CONF_ON_WIN, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "wake_word")], conf)
    for conf in config.get(CONF_ON_LOSE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "wake_word")], conf)
