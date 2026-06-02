import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import speaker, audio
from esphome.const import CONF_ID

from .. import fdaudio_ns, FdAudio

CONF_FDAUDIO_ID = "fdaudio_id"

FdAudioSpeaker = fdaudio_ns.class_(
    "FdAudioSpeaker", cg.Component, speaker.Speaker
)


def _set_stream_limits(config):
    # fdaudio shares ONE I2S clock between mic and speaker (full-duplex), so the
    # speaker must run at the same 16 kHz as the mic. Forcing min=max=16000 makes
    # the media_player/pipeline resample (e.g. 48k TTS -> 16k) before sending,
    # instead of playing 48k data at 16k (= 3x too slow).
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
            cv.GenerateID(): cv.declare_id(FdAudioSpeaker),
            cv.GenerateID(CONF_FDAUDIO_ID): cv.use_id(FdAudio),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    parent = await cg.get_variable(config[CONF_FDAUDIO_ID])
    cg.add(var.set_parent(parent))
