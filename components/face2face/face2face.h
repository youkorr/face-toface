#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <cstdint>
#include <vector>
#include <string>

namespace esphome {
namespace face2face {

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------
// Each media frame (one JPEG image, or one audio chunk) is split into UDP
// fragments small enough to avoid IP fragmentation (~1400 B payload). The
// receiver reassembles a frame from its fragments using (stream, frame_id).
//
// All multi-byte fields are little-endian (both peers are ESP32-P4 / LE).

static constexpr uint32_t F2F_MAGIC = 0x46324630;  // "F2F0"
static constexpr uint16_t F2F_MAX_PAYLOAD = 1400;

enum F2FStream : uint8_t {
  F2F_STREAM_VIDEO = 0,
  F2F_STREAM_AUDIO = 1,
};

enum F2FFlags : uint8_t {
  F2F_FLAG_LAST = 0x01,  // last fragment of this frame
  F2F_FLAG_KEY = 0x02,   // (reserved) full/keyframe marker
};

#pragma pack(push, 1)
struct F2FHeader {
  uint32_t magic;        // F2F_MAGIC
  uint8_t stream;        // F2FStream
  uint8_t flags;         // F2FFlags
  uint16_t frame_id;     // wrapping per-stream frame counter
  uint16_t frag_index;   // 0-based fragment index within the frame
  uint16_t frag_count;   // total number of fragments for this frame
  uint32_t frame_size;   // total payload bytes of the whole frame
  uint16_t payload_len;  // bytes of payload in *this* packet
};
#pragma pack(pop)

static constexpr size_t F2F_HEADER_SIZE = sizeof(F2FHeader);

// Reassembly buffer for one in-flight frame of a given stream.
struct FrameAssembler {
  uint16_t frame_id{0};
  uint32_t frame_size{0};
  uint16_t frag_count{0};
  uint16_t frags_seen{0};
  bool active{false};
  std::vector<uint8_t> data;        // sized to frame_size once known
  std::vector<bool> got;            // per-fragment received flag

  void reset(uint16_t id, uint32_t size, uint16_t count);
  bool complete() const { return active && frags_seen >= frag_count; }
};

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------
class Face2Face : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ---- YAML setters ----
  void set_peer_ip(const std::string &ip) { peer_ip_ = ip; }
  void set_video_port(uint16_t p) { video_port_ = p; }
  void set_audio_port(uint16_t p) { audio_port_ = p; }
  void set_resolution(uint16_t w, uint16_t h) { width_ = w; height_ = h; }
  void set_framerate(uint8_t fps) { framerate_ = fps; }
  void set_jpeg_quality(uint8_t q) { jpeg_quality_ = q; }
  void set_audio_enabled(bool e) { audio_enabled_ = e; }
  void set_audio_sample_rate(uint32_t r) { audio_sample_rate_ = r; }

  // ---- Call control (exposed to automations / LVGL buttons) ----
  void start_call();
  void stop_call();
  bool in_call() const { return in_call_; }

  // ---- Rendered remote frame access (for the display/LVGL hook) ----
  // Returns the most recently decoded remote frame as RGB565, or nullptr.
  const uint8_t *remote_rgb565() const { return remote_fb_.empty() ? nullptr : remote_fb_.data(); }
  uint16_t remote_width() const { return width_; }
  uint16_t remote_height() const { return height_; }
  bool has_new_remote_frame() { bool v = new_remote_frame_; new_remote_frame_ = false; return v; }

 protected:
  // ---- Networking ----
  bool open_sockets_();
  void close_sockets_();
  void poll_receive_();
  void send_frame_(F2FStream stream, const uint8_t *data, uint32_t len, int sock);
  void handle_packet_(const uint8_t *buf, size_t len, F2FStream expected);

  // ---- Capture / encode (esp_capture + hardware MJPEG) ----
  bool capture_init_();
  void capture_deinit_();
  // Pulls the next encoded frames (if ready) and ships them to the peer.
  void pump_tx_();

  // ---- Decode / render ----
  bool decoder_init_();
  void decoder_deinit_();
  // Hardware-decode a JPEG into remote_fb_ (RGB565). Returns true on success.
  bool decode_jpeg_(const uint8_t *jpeg, uint32_t len);

  // ---- Audio (I2S via esp_codec_dev) ----
  bool audio_init_();
  void audio_deinit_();
  void audio_play_(const uint8_t *pcm, uint32_t len);

  // ---- config ----
  std::string peer_ip_;
  uint16_t video_port_{9000};
  uint16_t audio_port_{9001};
  uint16_t width_{640};
  uint16_t height_{480};
  uint8_t framerate_{15};
  uint8_t jpeg_quality_{80};
  bool audio_enabled_{true};
  uint32_t audio_sample_rate_{16000};

  // ---- runtime state ----
  int video_sock_{-1};
  int audio_sock_{-1};
  bool in_call_{false};
  bool sockets_ready_{false};
  bool capture_ready_{false};
  bool decoder_ready_{false};

  uint16_t tx_video_frame_id_{0};
  uint16_t tx_audio_frame_id_{0};
  uint32_t last_tx_us_{0};

  FrameAssembler video_asm_;
  FrameAssembler audio_asm_;

  // Decoded remote video frame, RGB565 (width_*height_*2 bytes).
  std::vector<uint8_t> remote_fb_;
  bool new_remote_frame_{false};

  // Opaque handles for the Espressif managed components. Kept as void* so the
  // header does not need their (heavy) includes; the .cpp casts them.
  void *capture_handle_{nullptr};
  void *video_sink_{nullptr};
  void *audio_sink_{nullptr};
  void *jpeg_decoder_{nullptr};
  void *audio_dev_{nullptr};
};

}  // namespace face2face
}  // namespace esphome
