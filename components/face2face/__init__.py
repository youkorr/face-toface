"""ESPHome external component: face2face.

A FaceTime-like, peer-to-peer (direct IP) audio + video link between two
ESP32-P4 boards.

  * Video  : OV5647 (MIPI-CSI) -> hardware MJPEG encode -> UDP -> peer
             peer JPEG -> hardware JPEG decode -> LVGL canvas
  * Audio  : I2S mic -> encode -> UDP -> peer
             peer audio -> decode -> I2S speaker

The heavy lifting (capture, hardware MJPEG codec, audio codec) is delegated to
the Espressif managed components pulled in below.  This Python file only wires
the YAML schema to the C++ ``Face2Face`` component and registers those
dependencies with the ESP-IDF build.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]
AUTO_LOAD = []

CONF_PEER_IP = "peer_ip"
CONF_VIDEO_PORT = "video_port"
CONF_AUDIO_PORT = "audio_port"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_FRAMERATE = "framerate"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_CANVAS_BUFFER = "canvas_buffer"
CONF_ENABLE_AUDIO = "enable_audio"
CONF_AUDIO_SAMPLE_RATE = "audio_sample_rate"

face2face_ns = cg.esphome_ns.namespace("face2face")
Face2Face = face2face_ns.class_("Face2Face", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Face2Face),
        cv.Required(CONF_PEER_IP): cv.ipv4,
        cv.Optional(CONF_VIDEO_PORT, default=9000): cv.port,
        cv.Optional(CONF_AUDIO_PORT, default=9001): cv.port,
        cv.Optional(CONF_WIDTH, default=640): cv.int_range(min=160, max=1920),
        cv.Optional(CONF_HEIGHT, default=480): cv.int_range(min=120, max=1080),
        cv.Optional(CONF_FRAMERATE, default=15): cv.int_range(min=1, max=30),
        cv.Optional(CONF_JPEG_QUALITY, default=80): cv.int_range(min=10, max=100),
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

    # --- Espressif managed components (pulled from the component registry) ---
    # Pin the refs to versions that exist at build time; bump as needed.
    # esp_capture brings the GMF camera+audio capture pipeline and the
    # hardware MJPEG encoder; esp_video_codec brings the hardware JPEG
    # decoder; esp_video is the V4L2 MIPI-CSI driver for the OV5647.
    esp32.add_idf_component(name="espressif/esp_capture", ref="0.9.0")
    esp32.add_idf_component(name="espressif/esp_video_codec", ref="0.9.0")
    esp32.add_idf_component(name="espressif/esp_codec_dev", ref="1.3.4")

    # Larger socket buffers help with fragmented video over UDP.
    cg.add_define("FACE2FACE_VERSION", "0.1.0")
