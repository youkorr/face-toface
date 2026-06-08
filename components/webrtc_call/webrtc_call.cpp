#include "webrtc_call.h"
#include "esphome/core/log.h"
#include "ring_aac.h"

#ifdef WEBRTC_CALL_ENABLED
// Espressif esp-webrtc-solution headers (pulled in __init__.py).
#include "esp_webrtc.h"
#include "esp_webrtc_defaults.h"  // esp_signaling_get_apprtc_impl / esp_peer_get_default_impl
#include "esp_peer_default.h"
// GMF media (capture + render) — ported from videocall_demo media_sys.c/board.c.
#include "codec_init.h"
#include "codec_board.h"
#include "av_render.h"
#include "av_render_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4
// CSI camera + HW video codecs are P4-only in this stack.
#include "esp_video_init.h"
#include "esp_video_enc_default.h"
#include "esp_video_dec_default.h"
#endif
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
  ESP_LOGCONFIG(TAG, "  Board: %s", board_type_.c_str());
  if (has_video_)
    ESP_LOGCONFIG(TAG, "  Video: %ux%u @ %u fps (MJPEG), two-way", width_, height_, framerate_);
  else
    ESP_LOGCONFIG(TAG, "  Video: disabled (audio-only call)");
  ESP_LOGCONFIG(TAG, "  Audio: G.711A, full-duplex, AEC %s", aec_ ? "on" : "off");
  ESP_LOGCONFIG(TAG, "  STUN: %s", stun_server_.empty() ? "(none)" : stun_server_.c_str());
  ESP_LOGCONFIG(TAG, "  TURN: %s", turn_url_.empty() ? "(none)" : turn_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Ringtone: %s (embedded ring.aac, %u bytes)",
                ringtone_enabled_ ? "on" : "off", RING_AAC_LEN);
}

// ===========================================================================
// GMF media: camera + mic capture (MJPEG/G711) and speaker + LCD render.
// Ported from esp-webrtc-solution videocall_demo/media_sys.c.
//
// Both the camera and the LCD are OPTIONAL: when the selected codec_board has
// neither (e.g. ESP32_S3_BOX_3), the call runs audio-only. Audio is captured
// through the AEC source when aec_ is set, which removes the speaker echo that
// would otherwise feed back into the mic on a full-duplex call.
// ===========================================================================
bool WebrtcCall::media_init_() {
#ifdef WEBRTC_CALL_ENABLED
  // ---- Board init (port of board.c) ----
  // codec_board sets up the I2S + codec (ES8311/ES8388/ES7210) and the LCD, and
  // exposes get_record_handle()/get_playback_handle()/board_get_lcd_handle().
  // Either an inline definition (board_config:, parsed at runtime) or a board
  // name registered in codec_board (board_type:). The inline path lets the user
  // describe their exact Waveshare/Tab5 pinout in YAML without a predefined board.
  if (!board_config_.empty()) {
    codec_board_parse_all_config(board_config_.c_str());
    ESP_LOGI(TAG, "codec_board: parsed inline board_config (%u bytes)",
             static_cast<unsigned>(board_config_.size()));
  }
  set_codec_board_type(board_type_.c_str());
  codec_init_cfg_t ccfg = {};
  ccfg.reuse_dev = false;  // record + playback at the same time (full-duplex)
  init_codec(&ccfg);
  board_lcd_init();

  // ---- Register default codecs (media_sys_buildup) ----
  esp_audio_enc_register_default();
  esp_audio_dec_register_default();
#if CONFIG_IDF_TARGET_ESP32P4
  esp_video_enc_register_default();
  esp_video_dec_register_default();
#endif

  // ---- Capture: (optional) camera + mic ----
  esp_capture_video_src_if_t *vsrc = nullptr;
#if CONFIG_IDF_TARGET_ESP32P4
  camera_cfg_t cam = {};
  if (video_ && get_camera_cfg(&cam) == 0) {
    esp_video_init_csi_config_t csi = {};
    csi.sccb_config.i2c_handle = static_cast<i2c_master_bus_handle_t>(get_i2c_bus_handle(0));
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
    vsrc = esp_capture_new_video_v4l2_src(&v4l2);
    if (vsrc == nullptr) {
      ESP_LOGE(TAG, "video v4l2 src failed");
      return false;
    }
  }
#endif
  if (video_ && vsrc == nullptr)
    ESP_LOGW(TAG, "video requested but no camera on this board -> audio-only");

  // Audio source: AEC src (mic + speaker reference) for full-duplex without
  // echo, or the plain dev src. On ES7210 (S3-Box-3) the reference is the
  // 2nd TDM channel, so request 4 channels and mask mic+ref (1|2).
  esp_capture_audio_src_if_t *asrc = nullptr;
  if (aec_) {
    esp_capture_audio_aec_src_cfg_t aec_cfg = {};
    aec_cfg.record_handle = get_record_handle();
#if CONFIG_IDF_TARGET_ESP32S3
    aec_cfg.channel = 4;
    aec_cfg.channel_mask = 1 | 2;
#endif
    asrc = esp_capture_new_audio_aec_src(&aec_cfg);
  } else {
    esp_capture_audio_dev_src_cfg_t acfg = {};
    acfg.record_handle = get_record_handle();
    asrc = esp_capture_new_audio_dev_src(&acfg);
  }
  if (asrc == nullptr) {
    ESP_LOGE(TAG, "audio src failed");
    return false;
  }

  esp_capture_cfg_t cap_cfg = {};
  cap_cfg.sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO;
  cap_cfg.audio_src = asrc;
  cap_cfg.video_src = vsrc;  // NULL -> audio-only
  if (esp_capture_open(&cap_cfg, reinterpret_cast<esp_capture_handle_t *>(&capture_)) != 0) {
    ESP_LOGE(TAG, "esp_capture_open failed");
    return false;
  }

  // ---- Player: I2S speaker render + (optional) LCD video render ----
  i2s_render_cfg_t i2s_cfg = {};
  i2s_cfg.fixed_clock = true;
  i2s_cfg.play_handle = get_playback_handle();
  audio_render_handle_t arender = av_render_alloc_i2s_render(&i2s_cfg);
  if (arender == nullptr) {
    ESP_LOGE(TAG, "audio render failed");
    return false;
  }
  video_render_handle_t vrender = nullptr;
  void *lcd_handle = board_get_lcd_handle();
  if (vsrc != nullptr && lcd_handle != nullptr) {
    lcd_render_cfg_t lcd_cfg = {};
    lcd_cfg.lcd_handle = static_cast<esp_lcd_panel_handle_t>(lcd_handle);
    vrender = av_render_alloc_lcd_render(&lcd_cfg);
    if (vrender == nullptr) {
      ESP_LOGE(TAG, "video render failed");
      return false;
    }
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

  // With AEC the speaker must output 2 channels: the codec loops its right
  // channel back as the AEC reference (ES8311 on S3-Box-3). Force the render
  // output format so the reference lines up regardless of the incoming stream.
  if (aec_) {
    av_render_audio_frame_info_t aud_info = {};
    aud_info.sample_rate = 16000;
    aud_info.channel = 2;
    aud_info.bits_per_sample = 16;
    av_render_set_fixed_frame_info(static_cast<av_render_handle_t>(player_), &aud_info);
  }

  has_video_ = (vrender != nullptr);
  media_ready_ = true;
  ESP_LOGI(TAG, "GMF media ready (codec_board '%s', %s, AEC %s)",
           board_type_.c_str(), has_video_ ? "audio+video" : "audio-only",
           aec_ ? "on" : "off");
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
  // esp_peer tuning: data-channel cache big enough for MJPEG frames (video is
  // sent over the data channel in this stack, see video_over_data_channel).
  static esp_peer_default_cfg_t peer_default_cfg = {};
  peer_default_cfg.data_ch_cfg.send_cache_size = 400 * 1024;
  peer_default_cfg.data_ch_cfg.recv_cache_size = 400 * 1024;

  esp_webrtc_cfg_t cfg = {};
  // esp-webrtc keeps these as non-const char* (it does not modify them).
  cfg.signaling_cfg.signal_url = const_cast<char *>(signaling_url_.c_str());
  cfg.signaling_impl = esp_signaling_get_apprtc_impl();
  cfg.peer_impl = esp_peer_get_default_impl();

  // Audio: G.711A (both directions). Video: MJPEG (HW codec, frame-independent
  // -> loss tolerant), only when a camera+LCD are present.
  cfg.peer_cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_G711A;
  cfg.peer_cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;  // full-duplex audio
  if (has_video_) {
    cfg.peer_cfg.video_info.codec = ESP_PEER_VIDEO_CODEC_MJPEG;
    cfg.peer_cfg.video_info.width = width_;
    cfg.peer_cfg.video_info.height = height_;
    cfg.peer_cfg.video_info.fps = framerate_;
    cfg.peer_cfg.video_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;  // two-way video
    cfg.peer_cfg.video_over_data_channel = true;            // MJPEG over data ch
  }
  cfg.peer_cfg.enable_data_channel = true;
  cfg.peer_cfg.no_auto_reconnect = true;  // we drive connect via start_call()
  cfg.peer_cfg.extra_cfg = &peer_default_cfg;
  cfg.peer_cfg.extra_size = sizeof(peer_default_cfg);

  // ICE servers (STUN/TURN) for NAT traversal. Filled from YAML if provided.
  static esp_peer_ice_server_cfg_t ice[2] = {};
  int ice_n = 0;
  if (!stun_server_.empty()) {
    ice[ice_n].stun_url = const_cast<char *>(stun_server_.c_str());
    ice_n++;
  }
  if (!turn_url_.empty()) {
    ice[ice_n].stun_url = const_cast<char *>(turn_url_.c_str());
    ice[ice_n].user = const_cast<char *>(turn_user_.c_str());
    ice[ice_n].psw = const_cast<char *>(turn_pass_.c_str());
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

  // Hand our GMF capture + player to esp_webrtc. capture -> sent to peer,
  // player <- renders the peer's incoming audio (+video).
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
            // Peer audio now owns the player; silence the ringback. Signal the
            // ringtone task to stop (non-blocking; it tears down its AAC stream).
            self->ring_stopping_ = self->ring_playing_;
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
  // Ringback: loop ring.aac until the peer connects (stopped in the CONNECTED
  // event) or the call is hung up.
  if (ringtone_enabled_)
    play_ringtone(-1);
#endif
}

void WebrtcCall::hangup() {
#ifdef WEBRTC_CALL_ENABLED
  if (webrtc_ == nullptr)
    return;
  ESP_LOGI(TAG, "hangup: disabling peer connection");
  stop_ringtone();
  esp_webrtc_enable_peer_connection(static_cast<esp_webrtc_handle_t>(webrtc_), false);
  connected_ = false;
#endif
}

// ===========================================================================
// Ringtone: decode the embedded ring.aac (AAC) through av_render.
// Ported from doorbell_demo media_sys.c (music_play_thread / play_music).
// ===========================================================================
void WebrtcCall::ringtone_thread_(void *arg) {
#ifdef WEBRTC_CALL_ENABLED
  auto *self = static_cast<WebrtcCall *>(arg);
  auto player = static_cast<av_render_handle_t>(self->player_);

  // Tell av_render the upcoming raw stream is AAC.
  av_render_audio_info_t info = {};
  info.codec = AV_RENDER_AUDIO_CODEC_AAC;
  av_render_add_audio_stream(player, &info);

  const uint8_t *music = RING_AAC;
  const int music_size = static_cast<int>(RING_AAC_LEN);
  // duration: <0 -> loop until stop_ringtone(); 0 -> play one loop; >0 -> ms.
  int duration = self->ring_duration_;
  const bool forever = duration < 0;
  int pos = 0;

  while (!self->ring_stopping_) {
    uint32_t start_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // Feed one ADTS frame at a time (frame size from the ADTS header).
    int send_size = music_size - pos;
    const uint8_t *adts = music + pos;
    if (adts[0] != 0xFF) {
      send_size = 0;  // not an ADTS sync word -> end/garbage, restart loop
    } else {
      int frame_size = ((adts[3] & 0x03) << 11) | (adts[4] << 3) | (adts[5] >> 5);
      if (frame_size < send_size)
        send_size = frame_size;
    }
    if (send_size > 0) {
      av_render_audio_data_t adata = {};
      adata.data = const_cast<uint8_t *>(adts);
      adata.size = static_cast<uint32_t>(send_size);
      if (av_render_add_audio_data(player, &adata) != 0)
        break;
      pos += send_size;
    }

    if (pos >= music_size || send_size == 0) {
      pos = 0;  // end of clip -> rewind for the next loop
      if (!forever && duration == 0) {
        // "play once" (or timed run elapsed): wait for the render FIFO to
        // drain, then stop.
        av_render_fifo_stat_t stat = {};
        while (!self->ring_stopping_) {
          av_render_get_audio_fifo_level(player, &stat);
          if (stat.data_size > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
          }
          break;
        }
        break;
      }
      // forever or timed-but-not-yet-elapsed: loop again from the start.
    }

    uint32_t end_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!forever && duration > 0) {
      duration -= static_cast<int>(end_ms - start_ms);
      if (duration < 0)
        duration = 0;  // elapsed -> drain & stop at the next clip boundary
    }
  }

  av_render_reset(player);
  self->ring_stopping_ = false;
  self->ring_playing_ = false;
  vTaskDelete(nullptr);
#endif
}

void WebrtcCall::play_ringtone(int duration_ms) {
#ifdef WEBRTC_CALL_ENABLED
  if (player_ == nullptr) {
    ESP_LOGW(TAG, "play_ringtone: player not ready");
    return;
  }
  if (ring_playing_) {
    ESP_LOGD(TAG, "ringtone already playing, restarting");
    stop_ringtone();
  }
  ring_playing_ = true;
  ring_stopping_ = false;
  ring_duration_ = duration_ms;
  // Decoding AAC needs a roomy stack; run off the main loop.
  if (xTaskCreate(&WebrtcCall::ringtone_thread_, "ringtone", 6144, this, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "failed to start ringtone task");
    ring_playing_ = false;
  }
#endif
}

void WebrtcCall::stop_ringtone() {
#ifdef WEBRTC_CALL_ENABLED
  if (!ring_playing_)
    return;
  ring_stopping_ = true;
  // Wait (bounded) for the task to acknowledge and tear down its AAC stream.
  for (int i = 0; i < 100 && ring_stopping_; i++)
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
}

}  // namespace webrtc_call
}  // namespace esphome
