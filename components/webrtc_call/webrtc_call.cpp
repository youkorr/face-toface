#include "webrtc_call.h"
#include "esphome/core/log.h"

#ifdef WEBRTC_CALL_ENABLED
// Espressif esp-webrtc-solution headers (pulled in __init__.py).
#include "esp_webrtc.h"
#include "esp_peer_default.h"
#include "apprtc_signaling.h"
// GMF media (capture + render) — ported from videocall_demo media_sys.c/board.c.
#include "codec_init.h"
#include "codec_board.h"
#include "esp_video_init.h"
#include "av_render.h"
#include "av_render_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_audio_enc_default.h"
#include "esp_video_enc_default.h"
#include "esp_video_dec_default.h"
#include "esp_audio_dec_default.h"
#endif

namespace esphome {
namespace webrtc_call {

static const char *const TAG = "webrtc_call";

void WebrtcCall::setup() {
  ESP_LOGCONFIG(TAG, "Setting up WebRTC call (esp-webrtc)...");
#ifdef WEBRTC_CALL_ENABLED
  if (!media_init_()) {
    ESP_LOGE(TAG, "media (GMF capture/render) init failed");
    this->mark_failed();
    return;
  }
  if (!webrtc_init_()) {
    ESP_LOGE(TAG, "esp_webrtc init failed");
    this->mark_failed();
    return;
  }
#else
  ESP_LOGE(TAG, "WEBRTC_CALL_ENABLED not defined");
#endif
}

void WebrtcCall::loop() {
#ifdef WEBRTC_CALL_ENABLED
  // esp_webrtc runs its own tasks; nothing required here for now.
#endif
}

void WebrtcCall::dump_config() {
  ESP_LOGCONFIG(TAG, "webrtc_call:");
  ESP_LOGCONFIG(TAG, "  Signaling: %s", signaling_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Room: %s", room_.c_str());
  ESP_LOGCONFIG(TAG, "  Video: %ux%u @ %u fps (MJPEG)", width_, height_, framerate_);
  ESP_LOGCONFIG(TAG, "  STUN: %s", stun_server_.empty() ? "(none)" : stun_server_.c_str());
  ESP_LOGCONFIG(TAG, "  TURN: %s", turn_url_.empty() ? "(none)" : turn_url_.c_str());
}

// ===========================================================================
// GMF media: camera + mic capture (MJPEG/G711) and speaker + LCD render.
// Ported from esp-webrtc-solution videocall_demo/media_sys.c.
// ===========================================================================
bool WebrtcCall::media_init_() {
#ifdef WEBRTC_CALL_ENABLED
  // ---- Board init (port of board.c) ----
  // codec_board sets up the I2S + codec (ES8311/ES8388/ES7210) and the LCD, and
  // exposes get_record_handle()/get_playback_handle()/board_get_lcd_handle().
  // The board_type must match a codec_board definition for THIS hardware.
  set_codec_board_type(board_type_.c_str());
  codec_init_cfg_t ccfg = {};
  ccfg.reuse_dev = false;  // record + playback at the same time
  init_codec(&ccfg);
  board_lcd_init();

  // ---- Register default codecs (media_sys_buildup) ----
  esp_video_enc_register_default();
  esp_audio_enc_register_default();
  esp_video_dec_register_default();
  esp_audio_dec_register_default();

  // ---- Capture: camera (MIPI-CSI) + mic ----
  camera_cfg_t cam = {};
  if (get_camera_cfg(&cam) != 0) {
    ESP_LOGE(TAG, "get_camera_cfg failed");
    return false;
  }
  esp_video_init_csi_config_t csi = {};
  csi.sccb_config.i2c_handle = get_i2c_bus_handle(0);
  csi.sccb_config.freq = 100000;
  csi.reset_pin = cam.reset;
  csi.pwdn_pin = cam.pwr;
  esp_video_init_config_t cam_config = {};
  cam_config.csi = &csi;
  if (esp_video_init(&cam_config) != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init (camera) failed");
    return false;
  }
  esp_capture_video_v4l2_src_cfg_t v4l2 = {};
  v4l2.dev_name = "/dev/video0";
  v4l2.buf_count = 2;
  esp_capture_video_src_if_t *vsrc = esp_capture_new_video_v4l2_src(&v4l2);
  if (vsrc == nullptr) {
    ESP_LOGE(TAG, "video v4l2 src failed");
    return false;
  }
  esp_capture_audio_dev_src_cfg_t acfg = {};
  acfg.record_handle = get_record_handle();
  esp_capture_audio_src_if_t *asrc = esp_capture_new_audio_dev_src(&acfg);
  if (asrc == nullptr) {
    ESP_LOGE(TAG, "audio dev src failed");
    return false;
  }
  esp_capture_cfg_t cap_cfg = {};
  cap_cfg.sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO;
  cap_cfg.audio_src = asrc;
  cap_cfg.video_src = vsrc;
  if (esp_capture_open(&cap_cfg, reinterpret_cast<esp_capture_handle_t *>(&capture_)) != 0) {
    ESP_LOGE(TAG, "esp_capture_open failed");
    return false;
  }

  // ---- Player: I2S speaker render + LCD video render ----
  i2s_render_cfg_t i2s_cfg = {};
  i2s_cfg.fixed_clock = true;
  i2s_cfg.play_handle = get_playback_handle();
  audio_render_handle_t arender = av_render_alloc_i2s_render(&i2s_cfg);
  if (arender == nullptr) {
    ESP_LOGE(TAG, "audio render failed");
    return false;
  }
  lcd_render_cfg_t lcd_cfg = {};
  lcd_cfg.lcd_handle = board_get_lcd_handle();
  video_render_handle_t vrender = av_render_alloc_lcd_render(&lcd_cfg);
  if (vrender == nullptr) {
    ESP_LOGE(TAG, "video render failed");
    return false;
  }
  av_render_cfg_t render_cfg = {};
  render_cfg.audio_render = arender;
  render_cfg.video_render = vrender;
  render_cfg.audio_raw_fifo_size = 4096;
  render_cfg.audio_render_fifo_size = 6 * 1024;
  render_cfg.video_raw_fifo_size = 500 * 1024;
  render_cfg.allow_drop_data = false;
  player_ = av_render_open(&render_cfg);
  if (player_ == nullptr) {
    ESP_LOGE(TAG, "av_render_open failed");
    return false;
  }

  media_ready_ = true;
  ESP_LOGI(TAG, "GMF media ready (camera /dev/video0 + codec_board '%s')", board_type_.c_str());
  return true;
#else
  return false;
#endif
}

// ===========================================================================
// esp_webrtc: apprtc signaling + default peer (ICE) + media provider.
// ===========================================================================
bool WebrtcCall::webrtc_init_() {
#ifdef WEBRTC_CALL_ENABLED
  esp_webrtc_cfg_t cfg = {};
  cfg.signaling_cfg.signal_url = signaling_url_.c_str();
  cfg.signaling_impl = esp_signaling_get_apprtc_impl();
  cfg.peer_impl = esp_peer_get_default_impl();

  // Media formats: MJPEG video (HW codec, frame-independent -> loss tolerant)
  // and G.711A audio, matching videocall_demo.
  cfg.peer_cfg.video_info.codec = ESP_PEER_VIDEO_CODEC_MJPEG;
  cfg.peer_cfg.video_info.width = width_;
  cfg.peer_cfg.video_info.height = height_;
  cfg.peer_cfg.video_info.fps = framerate_;
  cfg.peer_cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_G711A;

  // ICE servers (STUN/TURN) for NAT traversal. Filled from YAML if provided.
  static esp_peer_ice_server_cfg_t ice[2] = {};
  int ice_n = 0;
  if (!stun_server_.empty()) {
    ice[ice_n].stun_url = stun_server_.c_str();
    ice_n++;
  }
  if (!turn_url_.empty()) {
    ice[ice_n].stun_url = turn_url_.c_str();
    ice[ice_n].user = turn_user_.c_str();
    ice[ice_n].psw = turn_pass_.c_str();
    ice_n++;
  }
  if (ice_n > 0) {
    cfg.peer_cfg.server_lists = ice;
    cfg.peer_cfg.server_num = ice_n;
  }

  if (esp_webrtc_open(&cfg, reinterpret_cast<esp_webrtc_handle_t *>(&webrtc_)) != 0) {
    ESP_LOGE(TAG, "esp_webrtc_open failed");
    return false;
  }

  // Hand our GMF capture + player to esp_webrtc.
  esp_webrtc_media_provider_t provider = {};
  provider.capture = static_cast<esp_capture_handle_t>(capture_);
  provider.player = static_cast<av_render_handle_t>(player_);
  esp_webrtc_set_media_provider(static_cast<esp_webrtc_handle_t>(webrtc_), &provider);

  esp_webrtc_set_event_handler(
      static_cast<esp_webrtc_handle_t>(webrtc_),
      [](esp_webrtc_event_t *event, void *ctx) -> int {
        auto *self = static_cast<WebrtcCall *>(ctx);
        switch (event->type) {
          case ESP_WEBRTC_EVENT_CONNECTED:
            self->connected_ = true;
            ESP_LOGI(TAG, "WebRTC connected");
            break;
          case ESP_WEBRTC_EVENT_DISCONNECTED:
          case ESP_WEBRTC_EVENT_CONNECT_FAILED:
            self->connected_ = false;
            ESP_LOGI(TAG, "WebRTC disconnected");
            break;
          default:
            break;
        }
        return 0;
      },
      this);

  // Manual peer connection (we enable it on start_call), then start signaling.
  esp_webrtc_enable_peer_connection(static_cast<esp_webrtc_handle_t>(webrtc_), false);
  if (esp_webrtc_start(static_cast<esp_webrtc_handle_t>(webrtc_)) != 0) {
    ESP_LOGE(TAG, "esp_webrtc_start failed");
    return false;
  }
  started_ = true;
  ESP_LOGI(TAG, "esp_webrtc started (room=%s)", room_.c_str());
  if (auto_connect_)
    start_call();
  return true;
#else
  return false;
#endif
}

void WebrtcCall::start_call() {
#ifdef WEBRTC_CALL_ENABLED
  if (webrtc_ == nullptr || !started_)
    return;
  ESP_LOGI(TAG, "start_call: enabling peer connection");
  esp_webrtc_enable_peer_connection(static_cast<esp_webrtc_handle_t>(webrtc_), true);
#endif
}

void WebrtcCall::hangup() {
#ifdef WEBRTC_CALL_ENABLED
  if (webrtc_ == nullptr)
    return;
  ESP_LOGI(TAG, "hangup: disabling peer connection");
  esp_webrtc_enable_peer_connection(static_cast<esp_webrtc_handle_t>(webrtc_), false);
  connected_ = false;
#endif
}

}  // namespace webrtc_call
}  // namespace esphome
