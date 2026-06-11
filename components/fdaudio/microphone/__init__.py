import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone, audio
from esphome.const import CONF_ID

from .. import fdaudio_ns, FdAudio

CONF_FDAUDIO_ID = "fdaudio_id"

FdAudioMicrophone = fdaudio_ns.class_(
    "FdAudioMicrophone", cg.Component, microphone.Microphone
)


def _set_stream_limits(config):
    # fdaudio mic always outputs 16-bit mono @ 16 kHz.
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
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(FdAudioMicrophone),
            cv.GenerateID(CONF_FDAUDIO_ID): cv.use_id(FdAudio),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    parent = await cg.get_variable(config[CONF_FDAUDIO_ID])
    cg.add(var.set_parent(parent))
