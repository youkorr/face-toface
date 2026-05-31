"""ESPHome external component: face2face.

A FaceTime-like, peer-to-peer (direct IP, UDP) audio + video link between two
ESP32-P4 boards. Designed to plug into the user's existing stack:

  * Video TX : esp_cam_sensor (OV5647, ``tab5_cam``) gives an RGB565 frame ->
               ESP32-P4 *hardware* JPEG encoder -> UDP -> peer.
  * Video RX : UDP -> ESP32-P4 *hardware* JPEG decoder -> RGB565 ->
               LVGL canvas (pushed by a small YAML lambda, exactly like the
               existing ``network_camera`` does).
  * Audio    : ESPHome ``microphone`` (ES7210) -> UDP -> peer ``speaker``
               (ES8311), raw 16-bit PCM.

Everything is direct device-to-device over UDP. No HTTP server, no broker.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone, speaker
from esphome.const import CONF_ID

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]

CONF_PEER_IP = "peer_ip"
CONF_VIDEO_PORT = "video_port"
CONF_AUDIO_PORT = "audio_port"
CONF_CAMERA_ID = "camera_id"
CONF_MICROPHONE_ID = "microphone_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_FRAMERATE = "framerate"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_ENABLE_AUDIO = "enable_audio"
CONF_AUDIO_SAMPLE_RATE = "audio_sample_rate"

face2face_ns = cg.esphome_ns.namespace("face2face")
Face2Face = face2face_ns.class_("Face2Face", cg.Component)

# Reference to the user's existing camera component so we can pull RGB565 frames.
esp_cam_ns = cg.esphome_ns.namespace("esp_cam_sensor")
MipiDSICamComponent = esp_cam_ns.class_("MipiDSICamComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Face2Face),
        cv.Required(CONF_PEER_IP): cv.ipv4,
        cv.Required(CONF_CAMERA_ID): cv.use_id(MipiDSICamComponent),
        cv.Optional(CONF_MICROPHONE_ID): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_VIDEO_PORT, default=9000): cv.port,
        cv.Optional(CONF_AUDIO_PORT, default=9001): cv.port,
        cv.Optional(CONF_WIDTH, default=640): cv.int_range(min=160, max=1920),
        cv.Optional(CONF_HEIGHT, default=480): cv.int_range(min=120, max=1080),
        cv.Optional(CONF_FRAMERATE, default=15): cv.int_range(min=1, max=30),
        cv.Optional(CONF_JPEG_QUALITY, default=40): cv.int_range(min=10, max=100),
        cv.Optional(CONF_ENABLE_AUDIO, default=True): cv.boolean,
        cv.Optional(CONF_AUDIO_SAMPLE_RATE, default=16000): cv.int_,
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
    cg.add(var.set_audio_enabled(config[CONF_ENABLE_AUDIO]))
    cg.add(var.set_audio_sample_rate(config[CONF_AUDIO_SAMPLE_RATE]))

    cam = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(var.set_camera(cam))

    if CONF_MICROPHONE_ID in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE_ID])
        cg.add(var.set_microphone(mic))
    if CONF_SPEAKER_ID in config:
        spk = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker(spk))

    # The ESP32-P4 hardware JPEG codec lives in the built-in esp_driver_jpeg
    # IDF component; no managed component needs to be pulled.
    cg.add_define("FACE2FACE_VERSION", "0.2.0")
