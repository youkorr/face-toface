#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

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
// Each media frame (one JPEG image, or one PCM audio chunk) is split into UDP
// fragments <= ~1400 B to avoid IP fragmentation. The receiver reassembles a
// frame from its fragments using (stream, frame_id) and drops stale partials.
static constexpr uint32_t F2F_MAGIC = 0x46324630;  // "F2F0"
static constexpr uint16_t F2F_MAX_PAYLOAD = 1400;

enum F2FStream : uint8_t {
  F2F_STREAM_VIDEO = 0,
  F2F_STREAM_AUDIO = 1,
};

enum F2FFlags : uint8_t {
  F2F_FLAG_LAST = 0x01,  // last fragment of this frame
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
  void set_camera(esp_cam_sensor::MipiDSICamComponent *cam) { camera_ = cam; }
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { spk_ = spk; }

  // ---- Call control (LVGL buttons / HA automations) ----
  void start_call();
  void stop_call();
  bool in_call() const { return in_call_; }

  // ---- Remote video access (pushed to an LVGL canvas by a YAML lambda) ----
  const uint8_t *remote_rgb565() const { return remote_fb_.empty() ? nullptr : remote_fb_.data(); }
  uint16_t remote_width() const { return width_; }
  uint16_t remote_height() const { return height_; }
  bool has_new_remote_frame() {
    bool v = new_remote_frame_;
    new_remote_frame_ = false;
    return v;
  }

 protected:
  // networking
  bool open_sockets_();
  void poll_receive_();
  void send_frame_(F2FStream stream, const uint8_t *data, uint32_t len, int sock);
  void handle_packet_(const uint8_t *buf, size_t len, F2FStream expected);

  // hardware JPEG codec (esp_driver_jpeg)
  bool jpeg_init_();
  void pump_video_tx_();
  bool decode_jpeg_(const uint8_t *jpeg, uint32_t len);

  // audio (ESPHome microphone/speaker)
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

  // peers
  esp_cam_sensor::MipiDSICamComponent *camera_{nullptr};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spk_{nullptr};

  // runtime
  int video_sock_{-1};
  int audio_sock_{-1};
  bool in_call_{false};
  bool sockets_ready_{false};
  bool jpeg_ready_{false};
  uint16_t tx_video_frame_id_{0};
  uint16_t tx_audio_frame_id_{0};
  uint32_t last_tx_us_{0};

  FrameAssembler video_asm_;
  FrameAssembler audio_asm_;

  std::vector<uint8_t> remote_fb_;  // decoded remote frame, RGB565
  bool new_remote_frame_{false};

  // hardware JPEG handles + DMA buffers (opaque; cast in .cpp)
  void *jpeg_enc_{nullptr};
  void *jpeg_dec_{nullptr};
  uint8_t *enc_in_{nullptr};   // DMA-capable RGB565 encoder input
  uint8_t *enc_out_{nullptr};  // DMA-capable JPEG output scratch
  size_t enc_out_cap_{0};
  uint8_t *dec_in_{nullptr};   // DMA-capable JPEG decoder input
  size_t dec_in_cap_{0};
  uint8_t *dec_out_{nullptr};  // DMA-capable RGB565 output
  size_t dec_out_cap_{0};
};

}  // namespace face2face
}  // namespace esphome
