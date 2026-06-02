import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import speaker
from esphome.const import CONF_ID

from .. import fdaudio_ns, FdAudio

CONF_FDAUDIO_ID = "fdaudio_id"

FdAudioSpeaker = fdaudio_ns.class_(
    "FdAudioSpeaker", cg.Component, speaker.Speaker
)

CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(FdAudioSpeaker),
        cv.GenerateID(CONF_FDAUDIO_ID): cv.use_id(FdAudio),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    parent = await cg.get_variable(config[CONF_FDAUDIO_ID])
    cg.add(var.set_parent(parent))
