#pragma once
// webrtc_call — ESPHome component wrapping Espressif's esp-webrtc-solution.
// Owns the camera/I2S/LCD via GMF (esp_capture / av_render); do not combine
// with esp_video / fdaudio / lvgl-on-MIPI in the same firmware.

#include "esphome/core/component.h"
#include "esphome/core/automation.h"

#include <string>
#include <cstdint>

namespace esphome {
namespace webrtc_call {

class WebrtcCall : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ---- YAML setters ----
  void set_signaling_url(const std::string &u) { signaling_url_ = u; }
  void set_room(const std::string &r) { room_ = r; }
  void set_board_type(const std::string &b) { board_type_ = b; }
  void set_board_config(const std::string &c) { board_config_ = c; }
  void set_resolution(uint16_t w, uint16_t h) { width_ = w; height_ = h; }
  void set_framerate(uint8_t f) { framerate_ = f; }
  void set_auto_connect(bool a) { auto_connect_ = a; }
  void set_ringtone(bool e) { ringtone_enabled_ = e; }
  void set_stun_server(const std::string &s) { stun_server_ = s; }
  void set_turn(const std::string &url, const std::string &user, const std::string &pass) {
    turn_url_ = url; turn_user_ = user; turn_pass_ = pass;
  }

  // ---- Call control (used by the automation actions) ----
  void start_call();  // enable the peer connection (join + connect)
  void hangup();      // disable the peer connection

  // ---- Ringtone (embedded ring.aac, decoded by av_render) ----
  // duration: ms to loop; 0 = play one loop; <0 = loop until stop_ringtone().
  void play_ringtone(int duration_ms = -1);
  void stop_ringtone();

  bool is_connected() const { return connected_; }

 protected:
  bool media_init_();   // GMF capture + render (ported from media_sys.c)
  bool webrtc_init_();  // esp_webrtc_open + signaling + media provider + start
  static void ringtone_thread_(void *arg);  // AAC feed loop (port of music_play_thread)

  // config
  std::string signaling_url_;
  std::string room_{"esp_room"};
  std::string board_type_{"ESP32_P4_DEV"};  // codec_board definition name
  std::string board_config_;                // inline codec_board definition (optional)
  uint16_t width_{320}, height_{240};
  uint8_t framerate_{15};
  bool auto_connect_{false};
  bool ringtone_enabled_{true};  // play embedded ring.aac as ringback
  std::string stun_server_, turn_url_, turn_user_, turn_pass_;

  // esp-webrtc handles (opaque; kept as void* to keep the header light)
  void *webrtc_{nullptr};        // esp_webrtc_handle_t
  void *capture_{nullptr};       // esp_capture_handle_t
  void *player_{nullptr};        // av_render_handle_t
  bool media_ready_{false};
  bool started_{false};
  volatile bool connected_{false};

  // ringtone playback state (port of doorbell_demo media_sys.c music_*)
  volatile bool ring_playing_{false};
  volatile bool ring_stopping_{false};
  int ring_duration_{-1};
};

// ---- Actions ----
template<typename... Ts> class StartCallAction : public Action<Ts...> {
 public:
  explicit StartCallAction(WebrtcCall *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_call(); }
 protected:
  WebrtcCall *parent_;
};

template<typename... Ts> class HangupAction : public Action<Ts...> {
 public:
  explicit HangupAction(WebrtcCall *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->hangup(); }
 protected:
  WebrtcCall *parent_;
};

template<typename... Ts> class RingAction : public Action<Ts...> {
 public:
  explicit RingAction(WebrtcCall *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(int, duration)
  void play(Ts... x) override {
    int d = -1;  // default: loop until stop_ringtone()
    if (this->duration_.has_value())
      d = this->duration_.value(x...);
    this->parent_->play_ringtone(d);
  }
 protected:
  WebrtcCall *parent_;
};

template<typename... Ts> class StopRingAction : public Action<Ts...> {
 public:
  explicit StopRingAction(WebrtcCall *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->stop_ringtone(); }
 protected:
  WebrtcCall *parent_;
};

}  // namespace webrtc_call
}  // namespace esphome
