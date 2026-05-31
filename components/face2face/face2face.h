#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/automation.h"

#include <cstdint>
#include <vector>
#include <string>

// Forward-declared ESPHome peers (full includes pulled in the .cpp).
namespace esphome {
namespace esp_cam_sensor {
class MipiDSICamComponent;
}
namespace microphone {
class Microphone;
}
namespace speaker {
class Speaker;
}
}  // namespace esphome

namespace esphome {
namespace face2face {

// ---------------------------------------------------------------------------
// Wire protocol  (little-endian; both peers are ESP32-P4)
// ---------------------------------------------------------------------------
static constexpr uint32_t F2F_MAGIC = 0x46324630;  // "F2F0"
static constexpr uint16_t F2F_MAX_PAYLOAD = 1400;

enum F2FStream : uint8_t {
  F2F_STREAM_VIDEO = 0,
  F2F_STREAM_AUDIO = 1,
  F2F_STREAM_PING = 2,   // header-only heartbeat (presence)
  F2F_STREAM_CTRL = 3,   // 1-byte call-signaling message (F2FCtrl)
};

enum F2FFlags : uint8_t {
  F2F_FLAG_LAST = 0x01,
};

// Call-signaling messages (absorbed from the PBX-lite intercom design).
enum F2FCtrl : uint8_t {
  CTRL_CALL = 1,     // INVITE: I want to call you
  CTRL_RING = 2,     // provisional: I am presenting your call locally
  CTRL_ANSWER = 3,   // final: I accepted
  CTRL_HANGUP = 4,   // end an established call
  CTRL_DECLINE = 5,  // reject during setup
};

// Finite state machine for a point-to-point call.
enum CallState : uint8_t {
  STATE_IDLE = 0,
  STATE_OUTGOING = 1,  // we dialed, waiting for answer
  STATE_RINGING = 2,   // peer is calling us, waiting for our answer
  STATE_STREAMING = 3, // full-duplex audio+video established
};

#pragma pack(push, 1)
struct F2FHeader {
  uint32_t magic;
  uint8_t stream;
  uint8_t flags;
  uint16_t frame_id;
  uint16_t frag_index;
  uint16_t frag_count;
  uint32_t frame_size;
  uint16_t payload_len;
};
#pragma pack(pop)
static constexpr size_t F2F_HEADER_SIZE = sizeof(F2FHeader);

struct FrameAssembler {
  uint16_t frame_id{0};
  uint32_t frame_size{0};
  uint16_t frag_count{0};
  uint16_t frags_seen{0};
  bool active{false};
  std::vector<uint8_t> data;
  std::vector<bool> got;

  void reset(uint16_t id, uint32_t size, uint16_t count);
  bool complete() const { return active && frags_seen >= frag_count; }
};

// ---------------------------------------------------------------------------
class Face2Face : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // ---- YAML setters ----
  void set_peer_ip(const std::string &ip) { peer_ip_ = ip; }
  void set_video_port(uint16_t p) { video_port_ = p; }
  void set_audio_port(uint16_t p) { audio_port_ = p; }
  void set_resolution(uint16_t w, uint16_t h) { width_ = w; height_ = h; }
  void set_framerate(uint8_t fps) { framerate_ = fps; }
  void set_jpeg_quality(uint8_t q) { jpeg_quality_ = q; }
  void set_audio_enabled(bool e) { audio_enabled_ = e; }
  void set_audio_sample_rate(uint32_t r) { audio_sample_rate_ = r; }
  void set_ring_timeout(uint32_t ms) { ring_timeout_ms_ = ms; }
  void set_auto_answer(bool a) { auto_answer_ = a; }
  void set_camera(esp_cam_sensor::MipiDSICamComponent *cam) { camera_ = cam; }
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { spk_ = spk; }

  // ---- Call control (the native signaling — no external intercom) ----
  void call();      // dial the configured peer
  void answer();    // accept an incoming (ringing) call
  void decline();   // reject an incoming call
  void hangup();    // end / cancel the call
  CallState call_state() const { return state_; }
  bool in_call() const { return state_ == STATE_STREAMING; }

  // ---- Triggers (automation hooks) ----
  void set_on_ringing(Trigger<> *t) { on_ringing_ = t; }
  void set_on_outgoing_call(Trigger<> *t) { on_outgoing_ = t; }
  void set_on_streaming(Trigger<> *t) { on_streaming_ = t; }
  void set_on_idle(Trigger<> *t) { on_idle_ = t; }

  // ---- Presence (heartbeat) ----
  bool peer_online() const {
    return last_peer_rx_ms_ != 0 && (millis() - last_peer_rx_ms_) < presence_timeout_ms_;
  }
  uint32_t peer_last_seen_ms() const { return last_peer_rx_ms_; }
  void set_presence_timeout(uint32_t ms) { presence_timeout_ms_ = ms; }

  // ---- Remote video access (pushed to LVGL by a YAML lambda) ----
  const uint8_t *remote_rgb565() const { return remote_fb_.empty() ? nullptr : remote_fb_.data(); }
  uint16_t remote_width() const { return remote_w_ ? remote_w_ : width_; }
  uint16_t remote_height() const { return remote_h_ ? remote_h_ : height_; }
  bool has_new_remote_frame() {
    bool v = new_remote_frame_;
    new_remote_frame_ = false;
    return v;
  }

 protected:
  // call FSM
  void set_state_(CallState s);
  void start_streaming_();  // bring up media + go STREAMING
  void go_idle_();          // tear down media + go IDLE
  void on_ctrl_(uint8_t type);
  void send_ctrl_(F2FCtrl type);

  // networking
  bool open_sockets_();
  void poll_receive_();
  void send_frame_(F2FStream stream, const uint8_t *data, uint32_t len, int sock);
  void handle_packet_(const uint8_t *buf, size_t len, F2FStream expected);
  void send_ping_();

  // hardware JPEG codec
  bool jpeg_init_();
  void pump_video_tx_();
  bool decode_jpeg_(const uint8_t *jpeg, uint32_t len);

  // audio
  void on_mic_data_(const std::vector<uint8_t> &data);
  void play_audio_(const uint8_t *pcm, uint32_t len);

  // config
  std::string peer_ip_;
  uint16_t video_port_{9000};
  uint16_t audio_port_{9001};
  uint16_t width_{640};
  uint16_t height_{480};
  uint8_t framerate_{15};
  uint8_t jpeg_quality_{40};
  bool audio_enabled_{true};
  uint32_t audio_sample_rate_{16000};
  uint32_t ring_timeout_ms_{30000};
  bool auto_answer_{false};

  // peers
  esp_cam_sensor::MipiDSICamComponent *camera_{nullptr};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spk_{nullptr};

  // triggers
  Trigger<> *on_ringing_{nullptr};
  Trigger<> *on_outgoing_{nullptr};
  Trigger<> *on_streaming_{nullptr};
  Trigger<> *on_idle_{nullptr};

  // runtime
  int video_sock_{-1};
  int audio_sock_{-1};
  CallState state_{STATE_IDLE};
  uint32_t state_since_ms_{0};
  bool sockets_ready_{false};
  bool jpeg_ready_{false};
  bool mic_started_{false};
  uint16_t tx_video_frame_id_{0};
  uint16_t tx_audio_frame_id_{0};
  uint32_t last_tx_us_{0};

  // presence
  uint32_t last_peer_rx_ms_{0};
  uint32_t last_ping_tx_ms_{0};
  uint32_t presence_timeout_ms_{4000};

  FrameAssembler video_asm_;
  FrameAssembler audio_asm_;

  std::vector<uint8_t> remote_fb_;
  uint16_t remote_w_{0};
  uint16_t remote_h_{0};
  bool new_remote_frame_{false};

  // hardware JPEG handles + DMA buffers
  void *jpeg_enc_{nullptr};
  void *jpeg_dec_{nullptr};
  uint8_t *enc_in_{nullptr};
  uint8_t *enc_out_{nullptr};
  size_t enc_out_cap_{0};
  uint8_t *dec_in_{nullptr};
  size_t dec_in_cap_{0};
  uint8_t *dec_out_{nullptr};
  size_t dec_out_cap_{0};
};

// ---- Automation actions (face2face.call / answer / hangup / decline) ----
template<typename... Ts> class CallAction : public Action<Ts...> {
 public:
  explicit CallAction(Face2Face *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->call(); }
 protected:
  Face2Face *parent_;
};
template<typename... Ts> class AnswerAction : public Action<Ts...> {
 public:
  explicit AnswerAction(Face2Face *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->answer(); }
 protected:
  Face2Face *parent_;
};
template<typename... Ts> class HangupAction : public Action<Ts...> {
 public:
  explicit HangupAction(Face2Face *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->hangup(); }
 protected:
  Face2Face *parent_;
};
template<typename... Ts> class DeclineAction : public Action<Ts...> {
 public:
  explicit DeclineAction(Face2Face *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->decline(); }
 protected:
  Face2Face *parent_;
};

}  // namespace face2face
}  // namespace esphome
