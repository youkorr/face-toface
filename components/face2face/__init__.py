"""ESPHome external component: face2face.

A self-contained, FaceTime-like, peer-to-peer (direct IP, UDP) audio + video
call between two ESP32-P4 boards. No external intercom dependency:

  * Video : esp_cam_sensor (OV5647) RGB565 -> ESP32-P4 hardware JPEG encoder ->
            UDP -> peer -> hardware JPEG decoder -> RGB565 -> LVGL canvas.
  * Audio : ESPHome microphone -> UDP -> peer speaker (raw 16-bit PCM mono).
  * Call signaling : native UDP control messages (INVITE/RING/ANSWER/HANGUP/
            DECLINE) with an IDLE/OUTGOING/RINGING/STREAMING state machine,
            exposed as triggers (on_ringing/on_outgoing_call/on_streaming/
            on_idle) and actions (call/answer/hangup/decline).
  * Presence : 1 Hz UDP heartbeat -> peer_online().
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import esp32, microphone, speaker, switch
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]

CONF_PEER_IP = "peer_ip"
CONF_VIDEO_PORT = "video_port"
CONF_AUDIO_PORT = "audio_port"
CONF_CAMERA_ID = "camera_id"
CONF_MICROPHONE_ID = "microphone_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_AMPLIFIER = "amplifier"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_FRAMERATE = "framerate"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_SWAP_COLORS = "swap_colors"
CONF_SCALE = "scale"
CONF_ENABLE_AUDIO = "enable_audio"
CONF_AUDIO_SAMPLE_RATE = "audio_sample_rate"
CONF_RING_TIMEOUT = "ring_timeout"
CONF_AUTO_ANSWER = "auto_answer"
CONF_RINGTONE = "ringtone"
CONF_ENABLE_AEC = "enable_aec"
CONF_AEC_MODE = "aec_mode"
CONF_AEC_FILTER_LENGTH = "aec_filter_length"
CONF_AUDIO_START_DELAY = "audio_start_delay"

# ESP-SR aec_mode_t values (from esp_aec.h).
AEC_MODES = {
    "sr_low_cost": 0,
    "sr_high_perf": 1,
    "voip_low_cost": 3,
    "voip_high_perf": 4,
    "fd_low_cost": 5,
    "fd_high_perf": 6,
}
CONF_ON_RINGING = "on_ringing"
CONF_ON_OUTGOING_CALL = "on_outgoing_call"
CONF_ON_STREAMING = "on_streaming"
CONF_ON_IDLE = "on_idle"

face2face_ns = cg.esphome_ns.namespace("face2face")
Face2Face = face2face_ns.class_("Face2Face", cg.Component)

CallAction = face2face_ns.class_("CallAction", automation.Action)
AnswerAction = face2face_ns.class_("AnswerAction", automation.Action)
HangupAction = face2face_ns.class_("HangupAction", automation.Action)
DeclineAction = face2face_ns.class_("DeclineAction", automation.Action)

# Reference to the user's existing camera component (RGB565 frame source).
esp_cam_ns = cg.esphome_ns.namespace("esp_cam_sensor")
MipiDSICamComponent = esp_cam_ns.class_("MipiDSICamComponent", cg.Component)


def _trigger():
    return automation.validate_automation(
        {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger.template())}
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Face2Face),
        cv.Optional(CONF_PEER_IP, default="0.0.0.0"): cv.ipv4address,
        cv.Required(CONF_CAMERA_ID): cv.use_id(MipiDSICamComponent),
        cv.Optional(CONF_MICROPHONE_ID): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
        # Optional speaker-amplifier (PA) enable switch. face2face turns it on for
        # the duration of a call and off afterwards -- like the media_player does
        # for TTS via on_announcement -- so call audio isn't left at line level.
        cv.Optional(CONF_AMPLIFIER): cv.use_id(switch.Switch),
        cv.Optional(CONF_VIDEO_PORT, default=9000): cv.port,
        cv.Optional(CONF_AUDIO_PORT, default=9001): cv.port,
        cv.Optional(CONF_WIDTH, default=640): cv.int_range(min=160, max=1920),
        cv.Optional(CONF_HEIGHT, default=480): cv.int_range(min=120, max=1080),
        cv.Optional(CONF_FRAMERATE, default=15): cv.int_range(min=1, max=60),
        cv.Optional(CONF_JPEG_QUALITY, default=40): cv.int_range(min=10, max=100),
        cv.Optional(CONF_SWAP_COLORS, default=True): cv.boolean,
        cv.Optional(CONF_SCALE, default=3): cv.int_range(min=1, max=8),
        cv.Optional(CONF_ENABLE_AUDIO, default=True): cv.boolean,
        cv.Optional(CONF_AUDIO_SAMPLE_RATE, default=16000): cv.int_,
        cv.Optional(CONF_RING_TIMEOUT, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_AUTO_ANSWER, default=False): cv.boolean,
        cv.Optional(CONF_RINGTONE, default=True): cv.boolean,
        cv.Optional(CONF_ENABLE_AEC, default=True): cv.boolean,
        cv.Optional(CONF_AEC_MODE, default="sr_low_cost"): cv.enum(AEC_MODES, lower=True),
        cv.Optional(CONF_AEC_FILTER_LENGTH, default=4): cv.int_range(min=1, max=8),
        cv.Optional(CONF_AUDIO_START_DELAY, default="1500ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ON_RINGING): _trigger(),
        cv.Optional(CONF_ON_OUTGOING_CALL): _trigger(),
        cv.Optional(CONF_ON_STREAMING): _trigger(),
        cv.Optional(CONF_ON_IDLE): _trigger(),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_peer_ip(str(config[CONF_PEER_IP])))
    cg.add(var.set_video_port(config[CONF_VIDEO_PORT]))
    cg.add(var.set_audio_port(config[CONF_AUDIO_PORT]))
    cg.add(var.set_resolution(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_framerate(config[CONF_FRAMERATE]))
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    cg.add(var.set_swap_colors(config[CONF_SWAP_COLORS]))
    cg.add(var.set_scale(config[CONF_SCALE]))
    cg.add(var.set_audio_enabled(config[CONF_ENABLE_AUDIO]))
    cg.add(var.set_audio_sample_rate(config[CONF_AUDIO_SAMPLE_RATE]))
    cg.add(var.set_ring_timeout(config[CONF_RING_TIMEOUT]))
    cg.add(var.set_auto_answer(config[CONF_AUTO_ANSWER]))
    cg.add(var.set_ringtone(config[CONF_RINGTONE]))
    cg.add(var.set_aec_enabled(config[CONF_ENABLE_AEC]))
    cg.add(var.set_aec_mode(config[CONF_AEC_MODE]))
    cg.add(var.set_aec_filter_length(config[CONF_AEC_FILTER_LENGTH]))
    cg.add(var.set_audio_start_delay(config[CONF_AUDIO_START_DELAY]))

    # Acoustic echo cancellation: pull Espressif ESP-SR and compile the AEC path
    # only when enabled (keeps the component dependency-free otherwise).
    #
    # esp-sr master depends on esp-dsp 1.8.0. This matches the project override
    # 'espressif/esp-dsp==1.8.0' and esp-dl >=1.7.0, so esp_afe / aec_nlp_level
    # compile and face_detection coexists. (1.8.0 only adds esp32s31 vs 1.7.0.)
    if config[CONF_ENABLE_AEC]:
        esp32.add_idf_component(
            name="esp-sr",
            repo="https://github.com/espressif/esp-sr",
            ref="master",
        )
        cg.add_define("FACE2FACE_USE_AEC")

    # Ringtone: the embedded ring.aac is decoded to PCM at runtime. Pull
    # Espressif's audio codec lib for the standalone AAC decoder (esp_aac_dec).
    esp32.add_idf_component(name="espressif/esp_audio_codec", ref="2.3.0")

    cam = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(var.set_camera(cam))
    if CONF_MICROPHONE_ID in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE_ID])
        cg.add(var.set_microphone(mic))
    if CONF_SPEAKER_ID in config:
        spk = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker(spk))
    if CONF_AMPLIFIER in config:
        amp = await cg.get_variable(config[CONF_AMPLIFIER])
        cg.add(var.set_amplifier(amp))

    for conf in config.get(CONF_ON_RINGING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        await automation.build_automation(trigger, [], conf)
        cg.add(var.set_on_ringing(trigger))
    for conf in config.get(CONF_ON_OUTGOING_CALL, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        await automation.build_automation(trigger, [], conf)
        cg.add(var.set_on_outgoing_call(trigger))
    for conf in config.get(CONF_ON_STREAMING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        await automation.build_automation(trigger, [], conf)
        cg.add(var.set_on_streaming(trigger))
    for conf in config.get(CONF_ON_IDLE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        await automation.build_automation(trigger, [], conf)
        cg.add(var.set_on_idle(trigger))

    cg.add_define("FACE2FACE_VERSION", "0.3.0")


# ---- Automation actions ----------------------------------------------------
F2F_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(Face2Face)})


@automation.register_action("face2face.call", CallAction, F2F_ACTION_SCHEMA, synchronous=True)
async def f2f_call_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("face2face.answer", AnswerAction, F2F_ACTION_SCHEMA, synchronous=True)
async def f2f_answer_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("face2face.hangup", HangupAction, F2F_ACTION_SCHEMA, synchronous=True)
async def f2f_hangup_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("face2face.decline", DeclineAction, F2F_ACTION_SCHEMA, synchronous=True)
async def f2f_decline_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
