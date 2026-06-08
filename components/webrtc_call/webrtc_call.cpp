#include "webrtc_call.h"
#include "esphome/core/log.h"

#ifdef WEBRTC_CALL_ENABLED
// Espressif esp-webrtc-solution headers (pulled in __init__.py).
#include "esp_webrtc.h"
#include "esp_peer_default.h"
#include "apprtc_signaling.h"
// GMF media (capture + render) — used to feed/play the WebRTC streams.
#include "esp_capture.h"
#include "av_render.h"
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
  // TODO (next step): port media_sys_buildup():
  //   - esp_video_enc_register_default(); esp_audio_enc_register_default();
  //     esp_video_dec_register_default(); esp_audio_dec_register_default();
  //   - build capture: create_video_source() (MIPI-CSI) + audio dev src +
  //     esp_capture_open() with a sink (MJPEG video + G711A audio) ->
  //     store capture_ (esp_capture_handle_t).
  //   - build player: av_render_alloc_i2s_render() + av_render_alloc_lcd_render()
  //     + av_render_open() -> store player_ (av_render_handle_t).
  // This needs the exact esp_capture / av_render APIs (next fetch + iterate).
  ESP_LOGW(TAG, "media_init_: GMF capture/render not yet ported (step 2)");
  media_ready_ = false;
  return false;
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
