#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/automation.h"
#include "esphome/components/switch/switch.h"

#include <cstdint>
#include <vector>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

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
  // Safe to call at runtime (e.g. from a Home Assistant text entity). Ignores
  // an empty value so a blank field never wipes a working address.
  void set_peer_ip(const std::string &ip) {
    if (!ip.empty())
      peer_ip_ = ip;
  }
  std::string get_peer_ip() const { return peer_ip_; }
  // ---- Address book (name -> host) for direct IP-to-IP calls --------------
  // host is a public IP (port-forwarded) or a DNS/DDNS hostname, resolved at
  // call time so a changing home IP behind a DuckDNS name still works.
  void add_contact(const std::string &name, const std::string &host) {
    contacts_.push_back(Contact{name, host});
  }
  // Resolve a contact's host, set it as the peer, and dial. Returns false if
  // the name is unknown or the host can't be resolved (peer left unchanged).
  bool call_contact(const std::string &name);
  void set_video_port(uint16_t p) { video_port_ = p; }
  void set_audio_port(uint16_t p) { audio_port_ = p; }
  void set_resolution(uint16_t w, uint16_t h) { width_ = w; height_ = h; }
  void set_framerate(uint8_t fps) { framerate_ = fps; }
  void set_jpeg_quality(uint8_t q) { jpeg_quality_ = q; }
  void set_scale(uint8_t s) { scale_ = s < 1 ? 1 : s; }
  // Hardware (PPA) downscale target. w>0 enables it; h==0 derives from the
  // camera aspect ratio. Takes precedence over the integer scale_ when w>0.
  void set_output_size(uint16_t w, uint16_t h) { out_width_ = w; out_height_ = h; }
  void set_swap_colors(bool s) { swap_colors_ = s; }
  void set_audio_enabled(bool e) { audio_enabled_ = e; }
  void set_audio_sample_rate(uint32_t r) { audio_sample_rate_ = r; }
  void set_aec_enabled(bool e) { aec_enabled_ = e; }
  void set_aec_filter_length(int n) { aec_filter_length_ = n; }
  void set_audio_start_delay(uint32_t ms) { audio_start_delay_ms_ = ms; }
  void set_aec_mode(int m) { aec_mode_ = m; }
  void set_ringtone(bool e) { ringtone_enabled_ = e; }
  void set_ring_timeout(uint32_t ms) { ring_timeout_ms_ = ms; }
  void set_auto_answer(bool a) { auto_answer_ = a; }
  void set_camera(esp_cam_sensor::MipiDSICamComponent *cam) { camera_ = cam; }
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { spk_ = spk; }
  void set_amplifier(switch_::Switch *amp) { amplifier_ = amp; }

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

  // ---- Audio diagnostics (for an LVGL debug page / sensors) ----
  // mic level = what WE send (your voice). spk level = what we PLAY (peer audio).
  // tx/rx = audio frames sent/received. If you hear yourself and rx is moving,
  // the peer is echoing you back (network round-trip); if rx is 0, it's local.
  int get_mic_level() const { return (int) dbg_mic_peak_; }
  int get_spk_level() const { return (int) dbg_spk_peak_; }
  uint32_t get_audio_tx() const { return dbg_tx_audio_; }
  uint32_t get_audio_rx() const { return dbg_rx_audio_; }
  // C6 link throughput (kbit/s, updated ~1 Hz during a call).
  int get_tx_kbps() const { return dbg_tx_kbps_; }
  int get_rx_kbps() const { return dbg_rx_kbps_; }
  const char *state_str() const {
    switch (state_) {
      case STATE_IDLE: return "IDLE";
      case STATE_OUTGOING: return "OUTGOING";
      case STATE_RINGING: return "RINGING";
      case STATE_STREAMING: return "STREAMING";
      default: return "?";
    }
  }

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
  void handle_packet_(const uint8_t *buf, size_t len, F2FStream expected, const char *src_ip);
  void send_ping_();

  // hardware JPEG codec
  bool jpeg_init_();
  void jpeg_deinit_();
  void pump_video_tx_();
  bool decode_jpeg_(const uint8_t *jpeg, uint32_t len);

  // Hardware pixel scaling via the P4's PPA (2D-DMA). Downscales an RGB565
  // source into dst on the JPEG/PPA accelerator instead of a CPU pixel loop,
  // which both frees the CPU and stops stealing PSRAM bandwidth from the JPEG
  // engine. Returns false (caller falls back to a CPU resize) if the PPA is
  // unavailable or the transaction fails. Registered/freed with the JPEG codec.
  bool ppa_scale_rgb565_(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh);

  // Video-TX task: capture + downscale + HW-JPEG encode + UDP send run in a
  // dedicated FreeRTOS task (not the main loop). This is what unlocks the frame
  // rate: the ESPHome main loop is shared with LVGL (canvas redraw), WiFi, etc.
  // and only ticks ~10-15 Hz, so running TX there capped the send rate AND
  // stalled poll_receive_() during each encode/send -> dropped RX fragments ->
  // 3-7 fps. With TX off the loop, the loop drains the RX socket continuously
  // and the encode overlaps LVGL/decode on the other core. encode (this task)
  // and decode (main loop) serialise on jpeg_mutex_ for the shared HW engine.
  static void video_tx_task_(void *arg);
  void start_video_tx_task_();
  void stop_video_tx_task_();

  // lazy media resources: allocated on call start, freed on hangup so RAM/PSRAM
  // stay free while idle on the LVGL UI.
  bool ensure_media_();
  void release_media_();

  // audio
  void on_mic_data_(const std::vector<uint8_t> &data);
  void play_audio_(const uint8_t *pcm, uint32_t len);

  // Ringtone: the embedded ring.aac (AAC) is decoded to PCM once (esp_aac_dec)
  // and looped on the speaker while OUTGOING (caller) or RINGING (callee).
  // Falls back to a synthesised beep if decoding is unavailable.
  void pump_ringtone_();
  bool decode_ringtone_();  // AAC -> cached PCM (lazy, runs once)

  // acoustic echo cancellation (ESP-SR esp_aec)
  bool aec_init_();
  void aec_deinit_();
  void ref_push_(const int16_t *d, size_t n);  // store far-end (speaker) samples
  void ref_pop_(int16_t *d, size_t n);         // fetch time-aligned reference
  // Resolve an IP-or-hostname to a numeric IPv4 string ("" on failure).
  std::string resolve_host_(const std::string &host);

  // config
  std::string peer_ip_;
  // True when peer_ip_ was learned from incoming traffic (not statically
  // configured / not set via a contact) -> released on idle so the next caller
  // can be a different board.
  bool peer_learned_{false};
  // Address book: name -> host (IP or DNS/DDNS), resolved when dialled.
  struct Contact {
    std::string name;
    std::string host;
  };
  std::vector<Contact> contacts_;
  uint16_t video_port_{9000};
  uint16_t audio_port_{9001};
  uint16_t width_{640};
  uint16_t height_{480};
  uint8_t framerate_{15};
  uint8_t jpeg_quality_{40};
  uint8_t scale_{1};  // downscale factor before JPEG encode (1,2,3,4...)
  uint16_t out_width_{0};   // PPA hardware resize target width (0 = use scale_)
  uint16_t out_height_{0};  // PPA resize height (0 = derive from camera aspect)
  bool swap_colors_{true};  // byte-swap RGB565 (HW JPEG decoder vs LVGL order)
  bool audio_enabled_{true};
  uint32_t audio_sample_rate_{16000};
  uint32_t ring_timeout_ms_{30000};
  bool auto_answer_{false};
  bool ringtone_enabled_{true};
  bool aec_enabled_{true};
  int aec_filter_length_{4};
  int aec_mode_{4};  // AEC_MODE_VOIP_HIGH_PERF
  uint32_t audio_start_delay_ms_{1500};  // wait for wake-word to free the I2S bus

  bool camera_started_{false};  // did we start camera streaming for this call?
  uint32_t audio_due_ms_{0};    // when to start mic+speaker (0 = not pending)
  uint8_t audio_retries_{0};    // bounded retries for the mic handoff

  // peers
  esp_cam_sensor::MipiDSICamComponent *camera_{nullptr};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spk_{nullptr};
  switch_::Switch *amplifier_{nullptr};  // PA enable, on during a call

  // Audio diagnostics (mic = sent, spk = received/played; tx/rx frame counters).
  volatile int32_t dbg_mic_peak_{0};
  volatile int32_t dbg_spk_peak_{0};
  volatile uint32_t dbg_tx_audio_{0};
  volatile uint32_t dbg_rx_audio_{0};
  // Throughput meter (C6 link): bytes since last tick + computed kbit/s.
  uint32_t thru_tx_bytes_{0};
  uint32_t thru_rx_bytes_{0};
  uint32_t last_thru_ms_{0};
  int dbg_tx_kbps_{0};
  int dbg_rx_kbps_{0};

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
  uint32_t last_enc_warn_ms_{0};  // throttle encode-error logs (per-frame)

  // JPEG codec mutex: serialises the video-TX task's encode with the main loop's
  // decode on the single shared HW JPEG peripheral.
  SemaphoreHandle_t jpeg_mutex_{nullptr};

  // Video-TX task handle + run flag (created on call start, joined on hangup).
  TaskHandle_t tx_task_handle_{nullptr};
  volatile bool tx_task_run_{false};

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

  // AEC runtime
  void *aec_handle_{nullptr};
  bool aec_ready_{false};
  int aec_chunk_{0};            // samples per aec_process() call
  int16_t *aec_in_{nullptr};    // 16-byte aligned scratch (mic / ref / out)
  int16_t *aec_ref_{nullptr};
  int16_t *aec_out_{nullptr};
  std::vector<int16_t> mic_acc_;   // accumulates mic samples to chunk boundaries
  std::vector<int16_t> send_acc_;  // accumulates AEC output to ship
  std::vector<int16_t> ref_buf_;   // ring buffer of far-end (reference) samples
  size_t ref_cap_{0};
  size_t ref_head_{0};
  size_t ref_count_{0};
  uint32_t last_spk_ms_{0};  // last time the speaker played (AEC gating)
  uint32_t ring_phase_{0};       // sample counter for tone synthesis (fallback beep)
  uint32_t last_ring_ms_{0};     // pacing for ringtone chunks
  bool ring_spk_started_{false}; // did we start the speaker for the ringtone?
  // Decoded ring.aac PCM (mono int16), looped during the ringing phase.
  std::vector<int16_t> ring_pcm_;
  uint32_t ring_pcm_rate_{0};    // sample rate reported by the AAC decoder
  size_t ring_pcm_pos_{0};       // playback cursor into ring_pcm_
  bool ring_decoded_{false};     // decode attempted (success or give-up)

  // hardware JPEG handles + DMA buffers
  void *jpeg_enc_{nullptr};
  void *jpeg_dec_{nullptr};
  void *ppa_client_{nullptr};  // PPA SRM client for hardware downscale (per-call)
  uint8_t *enc_in_{nullptr};
  size_t enc_in_cap_{0};
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
  TEMPLATABLE_VALUE(std::string, contact)  // optional: dial a named contact
  void play(const Ts &...x) override {
    if (this->contact_.has_value())
      this->parent_->call_contact(this->contact_.value(x...));
    else
      this->parent_->call();
  }
 protected:
  Face2Face *parent_;
};
template<typename... Ts> class AnswerAction : public Action<Ts...> {
 public:
  explicit AnswerAction(Face2Face *parent) : parent_(parent) {}
  void play(const Ts &...x) override { ((void) x, ...); this->parent_->answer(); }
 protected:
  Face2Face *parent_;
};
template<typename... Ts> class HangupAction : public Action<Ts...> {
 public:
  explicit HangupAction(Face2Face *parent) : parent_(parent) {}
  void play(const Ts &...x) override { ((void) x, ...); this->parent_->hangup(); }
 protected:
  Face2Face *parent_;
};
template<typename... Ts> class DeclineAction : public Action<Ts...> {
 public:
  explicit DeclineAction(Face2Face *parent) : parent_(parent) {}
  void play(const Ts &...x) override { ((void) x, ...); this->parent_->decline(); }
 protected:
  Face2Face *parent_;
};

}  // namespace face2face
}  // namespace esphome
