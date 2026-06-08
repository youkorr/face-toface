"""webrtc_call — ESPHome wrapper around Espressif's esp-webrtc-solution.

Cross-network (FaceTime-like) audio+video calls between ESP32-P4 boards using
real WebRTC: apprtc signaling, ICE/STUN/TURN NAT traversal, MJPEG video +
G.711 audio, captured/rendered via Espressif's GMF (esp_capture / av_render).

IMPORTANT — hardware ownership:
  esp-webrtc OWNS the camera (CSI), the I2S audio codec, and the LCD via GMF.
  It therefore CONFLICTS with esp_video / fdaudio / lvgl, which own the same
  hardware. Use this component in a DEDICATED "WebRTC mode" firmware: do NOT
  also declare esp_video / fdaudio / a MIPI display in the same config.

Build is multi-step: this is the scaffold (deps + signaling + lifecycle). The
GMF media bring-up is ported from videocall_demo/media_sys.c.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import esp32
from esphome.const import CONF_ID, CONF_WIDTH, CONF_HEIGHT

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]

CONF_SIGNALING_URL = "signaling_url"
CONF_ROOM = "room"
CONF_BOARD_TYPE = "board_type"
CONF_BOARD_CONFIG = "board_config"
CONF_FRAMERATE = "framerate"
CONF_STUN_SERVER = "stun_server"
CONF_TURN_URL = "turn_url"
CONF_TURN_USER = "turn_username"
CONF_TURN_PASSWORD = "turn_password"
CONF_AUTO_CONNECT = "auto_connect"
CONF_RINGTONE = "ringtone"
CONF_DURATION = "duration"

webrtc_call_ns = cg.esphome_ns.namespace("webrtc_call")
WebrtcCall = webrtc_call_ns.class_("WebrtcCall", cg.Component)

StartCallAction = webrtc_call_ns.class_("StartCallAction", automation.Action)
HangupAction = webrtc_call_ns.class_("HangupAction", automation.Action)
RingAction = webrtc_call_ns.class_("RingAction", automation.Action)
StopRingAction = webrtc_call_ns.class_("StopRingAction", automation.Action)

# esp-webrtc-solution: the components live as paths inside the repo.
WEBRTC_REPO = "https://github.com/espressif/esp-webrtc-solution"
WEBRTC_REF = "main"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebrtcCall),
        # apprtc signaling server URL (self-hosted, e.g. on Unraid).
        cv.Required(CONF_SIGNALING_URL): cv.string,
        # Room id: both boards joining the same room get connected.
        cv.Optional(CONF_ROOM, default="esp_room"): cv.string,
        # codec_board definition name (sets the I2S codec + LCD + camera pins).
        # Must match a board registered in the codec_board component for YOUR
        # hardware (Waveshare/Tab5 -> custom definition needed).
        cv.Optional(CONF_BOARD_TYPE, default="ESP32_P4_DEV"): cv.string,
        # Inline codec_board definition (text, codec_board format). When set,
        # it is parsed at runtime (codec_board_parse_all_config) so you describe
        # YOUR hardware (i2c/i2s/codec/camera/lcd) without a predefined board.
        cv.Optional(CONF_BOARD_CONFIG): cv.string,
        cv.Optional(CONF_WIDTH, default=320): cv.int_range(min=160, max=1280),
        cv.Optional(CONF_HEIGHT, default=240): cv.int_range(min=120, max=720),
        cv.Optional(CONF_FRAMERATE, default=15): cv.int_range(min=1, max=30),
        # NAT traversal (coturn on your server). STUN is enough on many networks;
        # TURN is the relay fallback for symmetric NATs.
        cv.Optional(CONF_STUN_SERVER): cv.string,
        cv.Optional(CONF_TURN_URL): cv.string,
        cv.Optional(CONF_TURN_USER): cv.string,
        cv.Optional(CONF_TURN_PASSWORD): cv.string,
        # If true, connect the peer as soon as the room is joined (else manual
        # via the start_call action).
        cv.Optional(CONF_AUTO_CONNECT, default=False): cv.boolean,
        # Play the embedded ring.aac (AAC, decoded by av_render) as ringback when
        # a call starts; auto-stopped on connect/hangup. Set false to silence it.
        cv.Optional(CONF_RINGTONE, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_signaling_url(config[CONF_SIGNALING_URL]))
    cg.add(var.set_room(config[CONF_ROOM]))
    cg.add(var.set_board_type(config[CONF_BOARD_TYPE]))
    if CONF_BOARD_CONFIG in config:
        cg.add(var.set_board_config(config[CONF_BOARD_CONFIG]))
    cg.add(var.set_resolution(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_framerate(config[CONF_FRAMERATE]))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    cg.add(var.set_ringtone(config[CONF_RINGTONE]))
    if CONF_STUN_SERVER in config:
        cg.add(var.set_stun_server(config[CONF_STUN_SERVER]))
    if CONF_TURN_URL in config:
        cg.add(var.set_turn(
            config[CONF_TURN_URL],
            config.get(CONF_TURN_USER, ""),
            config.get(CONF_TURN_PASSWORD, ""),
        ))

    # --- Espressif esp-webrtc-solution components (pulled from the repo) ---
    # H.264 HW codec (registry) + the WebRTC stack (path components in the repo).
    esp32.add_idf_component(name="espressif/esp_h264", ref="1.0.4")
    for name, path in (
        ("media_lib_sal", "components/media_lib_sal"),
        ("esp_capture", "components/esp_capture"),
        ("av_render", "components/av_render"),
        ("codec_board", "components/codec_board"),
        ("esp_peer", "components/esp_peer"),
        ("esp_webrtc", "components/esp_webrtc"),
        ("apprtc_signal", "components/esp_webrtc/impl/apprtc_signal"),
    ):
        esp32.add_idf_component(name=name, repo=WEBRTC_REPO, ref=WEBRTC_REF, path=path)

    cg.add_define("WEBRTC_CALL_ENABLED")


# ---- Automation actions ----------------------------------------------------
ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(WebrtcCall)})


@automation.register_action("webrtc_call.start", StartCallAction, ACTION_SCHEMA, synchronous=True)
async def start_call_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("webrtc_call.hangup", HangupAction, ACTION_SCHEMA, synchronous=True)
async def hangup_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


# Ring action: optional `duration` (ms). Omitted -> loop until stop_ring/connect.
RING_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(WebrtcCall),
        cv.Optional(CONF_DURATION): cv.templatable(cv.int_),
    }
)


@automation.register_action("webrtc_call.ring", RingAction, RING_SCHEMA)
async def ring_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    if CONF_DURATION in config:
        templ = await cg.templatable(config[CONF_DURATION], args, int)
        cg.add(var.set_duration(templ))
    return var


@automation.register_action("webrtc_call.stop_ring", StopRingAction, ACTION_SCHEMA, synchronous=True)
async def stop_ring_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
