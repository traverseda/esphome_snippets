"""Voice front end: the processing chain between raw mics and wake word / STT.

Phase 2 of docs/rewrite-plan.md. Every stage can be switched off at runtime,
and a debug tap exposes the signal at any stage, so each one has to earn its
place by measurement. Chain, in order:

    mic mix -> AEC (vs. the hardware reference) -> NS -> AGC + limiter

(The hardware high-pass runs before all of this, in the ES7210; see audio_io.)
"""

import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODE

from esphome.components.audio_io import CONF_AUDIO_IO_ID, AudioIO

CODEOWNERS = ["@traverseda"]
DEPENDENCIES = ["audio_io"]

CONF_VOICE_FRONTEND_ID = "voice_frontend_id"
CONF_MIC_MIX = "mic_mix"
CONF_AEC = "aec"
CONF_NS = "ns"
CONF_AGC = "agc"
CONF_ENABLED = "enabled"
CONF_FILTER_LENGTH = "filter_length"
CONF_NLP_LEVEL = "nlp_level"
CONF_TARGET_LEVEL_DBFS = "target_level_dbfs"
CONF_COMPRESSION_GAIN_DB = "compression_gain_db"
CONF_LIMITER = "limiter"
CONF_TAP = "tap"
CONF_INPUT_GAIN_DB = "input_gain_db"
CONF_MAKEUP_GAIN_DB = "makeup_gain_db"

# Same esp-sr the old firmware pinned: 2.6.0 cost ~17 KB of internal heap.
ESP_SR_REF = "2.4.7"

voice_frontend_ns = cg.esphome_ns.namespace("voice_frontend")
VoiceFrontend = voice_frontend_ns.class_("VoiceFrontend", cg.Component)
MicMix = voice_frontend_ns.enum("MicMix", is_class=True)
Tap = voice_frontend_ns.enum("Tap", is_class=True)

MIC_MIXES = {"mic1": MicMix.MIC1, "mic2": MicMix.MIC2, "average": MicMix.AVERAGE}
TAPS = {"mix": Tap.MIX, "aec": Tap.AEC, "ns": Tap.NS, "output": Tap.OUTPUT}
AEC_MODES = {"sr_low_cost": 0, "sr_high_perf": 1, "voip_low_cost": 3, "voip_high_perf": 4}
NLP_LEVELS = {"normal": 0, "aggressive": 1, "very_aggressive": 2}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VoiceFrontend),
        cv.GenerateID(CONF_AUDIO_IO_ID): cv.use_id(AudioIO),
        # How the two mics become one channel. "average" is a broadside
        # delay-and-sum: up to +3 dB against each mic's uncorrelated self-noise.
        cv.Optional(CONF_MIC_MIX, default="average"): cv.enum(MIC_MIXES, lower=True),
        # Digital gain on the mic mix, applied while converting 24 -> 16 bit so
        # no resolution is lost. It runs before AEC, so it must leave headroom
        # for the speaker's own echo: on this board that peaks around -12 dBFS
        # raw at 60% volume, so more than ~6 dB here clips it.
        cv.Optional(CONF_INPUT_GAIN_DB, default=6.0): cv.float_range(min=0.0, max=42.0),
        # Gain after AEC, before NS. Input + makeup is the total; 30 dB total was
        # the speech level picked by ear on 2026-10-10.
        cv.Optional(CONF_MAKEUP_GAIN_DB, default=24.0): cv.float_range(min=0.0, max=42.0),
        cv.Optional(CONF_AEC, default={}): cv.Schema(
            {
                cv.Optional(CONF_ENABLED, default=True): cv.boolean,
                cv.Optional(CONF_MODE, default="sr_low_cost"): cv.enum(AEC_MODES, lower=True),
                cv.Optional(CONF_FILTER_LENGTH, default=4): cv.int_range(min=1, max=8),
                cv.Optional(CONF_NLP_LEVEL, default="aggressive"): cv.enum(NLP_LEVELS, lower=True),
            }
        ),
        cv.Optional(CONF_NS, default={}): cv.Schema(
            {
                cv.Optional(CONF_ENABLED, default=True): cv.boolean,
                # 0 mild, 1 medium, 2 aggressive (esp-sr / WebRTC NS).
                cv.Optional(CONF_MODE, default=2): cv.int_range(min=0, max=2),
            }
        ),
        cv.Optional(CONF_AGC, default={}): cv.Schema(
            {
                cv.Optional(CONF_ENABLED, default=True): cv.boolean,
                # Speech envelope target, in dB below full scale (WebRTC "targetLevelDbfs").
                cv.Optional(CONF_TARGET_LEVEL_DBFS, default=3): cv.int_range(min=0, max=31),
                # Maximum gain the AGC may apply, dB.
                cv.Optional(CONF_COMPRESSION_GAIN_DB, default=24): cv.int_range(min=0, max=90),
                cv.Optional(CONF_LIMITER, default=True): cv.boolean,
            }
        ),
        # Which stage the debug channel (channel 1 of the microphone) carries.
        cv.Optional(CONF_TAP, default="mix"): cv.enum(TAPS, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    add_idf_component(name="espressif/esp-sr", ref=ESP_SR_REF)
    add_idf_component(name="espressif/esp-dsp", ref="^1.8.0")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_AUDIO_IO_ID])
    cg.add(var.set_audio_io(hub))

    cg.add(var.set_mic_mix(config[CONF_MIC_MIX]))
    cg.add(var.set_input_gain_db(config[CONF_INPUT_GAIN_DB]))
    cg.add(var.set_makeup_gain_db(config[CONF_MAKEUP_GAIN_DB]))
    aec = config[CONF_AEC]
    cg.add(var.configure_aec(aec[CONF_MODE], aec[CONF_FILTER_LENGTH], aec[CONF_NLP_LEVEL]))
    cg.add(var.set_aec_enabled(aec[CONF_ENABLED]))
    ns = config[CONF_NS]
    cg.add(var.set_ns_mode(ns[CONF_MODE]))
    cg.add(var.set_ns_enabled(ns[CONF_ENABLED]))
    agc = config[CONF_AGC]
    cg.add(var.set_agc_config(agc[CONF_TARGET_LEVEL_DBFS], agc[CONF_COMPRESSION_GAIN_DB], agc[CONF_LIMITER]))
    cg.add(var.set_agc_enabled(agc[CONF_ENABLED]))
    cg.add(var.set_tap(config[CONF_TAP]))
