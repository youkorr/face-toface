import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone
from esphome.const import CONF_ID

from .. import fdaudio_ns, FdAudio

CONF_FDAUDIO_ID = "fdaudio_id"

FdAudioMicrophone = fdaudio_ns.class_(
    "FdAudioMicrophone", cg.Component, microphone.Microphone
)

CONFIG_SCHEMA = microphone.MICROPHONE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(FdAudioMicrophone),
        cv.GenerateID(CONF_FDAUDIO_ID): cv.use_id(FdAudio),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    parent = await cg.get_variable(config[CONF_FDAUDIO_ID])
    cg.add(var.set_parent(parent))
