#pragma once
// fdaudio — Full-duplex I2S audio for ESP32-P4, built directly on Espressif
// APIs (esp_driver_i2s + esp_codec_dev + esp-sr AEC). No third-party component.
//
// One I2S port runs TX and RX simultaneously (i2s_new_channel with both
// handles => full-duplex, shared clock). The same I2S data interface is shared
// by the ES8311/ES8388 (speaker, OUT) and ES7210 (mic, IN) codec devices.
//
// Exposes a standard ESPHome microphone + speaker so voice_assistant,
// media_player and face2face can use them unchanged. AEC (esp-sr) runs on the
// mic path using the last played speaker frame as the echo reference.

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <vector>
#include <cstdint>

// ESP-IDF I2S driver (local, no managed component)
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"

namespace esphome {
namespace fdaudio {

enum OutputCodec : uint8_t {
  OUT_ES8311 = 0,
  OUT_ES8388 = 1,
};

// The shared full-duplex engine. One per device.
class FdAudio : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- YAML setters ----
  void set_pins(int mclk, int bclk, int lrclk, int din, int dout) {
    mclk_pin_ = mclk; bclk_pin_ = bclk; lrclk_pin_ = lrclk; din_pin_ = din; dout_pin_ = dout;
  }
  void set_sample_rate(uint32_t r) { mic_rate_ = r; }              // mic output (ESPHome-facing)
  void set_codec_sample_rate(uint32_t r) { codec_rate_ = r; }      // actual I2S/codec clock
  void set_i2c_port(int p) { i2c_port_ = p; }
  void set_output_codec(OutputCodec c) { out_codec_ = c; }
  void set_codec_addrs(uint8_t out_addr, uint8_t in_addr) { out_addr_ = out_addr; in_addr_ = in_addr; }
  void set_mic_gain_db(float g) { mic_gain_db_ = g; }
  void set_mic_channels(uint8_t m) { mic_channels_ = m; }
  void set_mic_digital_gain(float g) { mic_digital_gain_ = g; }
  void set_noise_gate(int t) { noise_gate_thresh_ = t; }
  void set_out_volume(int v) { out_volume_ = v; }
  void set_use_mclk(bool u) { use_mclk_ = u; }
  void set_aec_enabled(bool e) { aec_enabled_ = e; }

  // ---- Engine control (ref-counted: started while any consumer is active) ----
  bool engine_start();
  void engine_stop();
  bool engine_running() const { return running_; }

  // ---- Mic read (16-bit mono at mic_rate_) / speaker write (at codec_rate_) ----
  // Returns bytes actually read/written. Mic data is AEC-cleaned if enabled.
  size_t read_mic(uint8_t *dst, size_t len);
  void write_speaker(const uint8_t *src, size_t len);

 protected:
  bool init_i2s_();
  bool init_codecs_();
  bool init_aec_();
  void deinit_();
  void run_aec_(int16_t *mic, size_t samples);  // in-place on mic buffer

  // config
  int mclk_pin_{-1}, bclk_pin_{-1}, lrclk_pin_{-1}, din_pin_{-1}, dout_pin_{-1};
  uint32_t mic_rate_{16000};     // rate exposed to ESPHome (mic output, VA/face2face)
  uint32_t codec_rate_{48000};   // actual I2S + codec clock (matches working board config)
  std::vector<int16_t> mic_scratch_;  // codec-rate read buffer before decimation
  float mic_digital_gain_{1.0f};      // software boost for weak mic (AGC-lite)
  // Lightweight software noise gate (0 = disabled). No buffers, a few float
  // ops/sample. gate_env_ tracks the signal envelope; gate_gain_ is the smoothed
  // applied gain (opens fast, closes slowly to avoid clicks).
  int noise_gate_thresh_{0};
  float gate_env_{0.0f};
  float gate_gain_{1.0f};
  int i2c_port_{0};
  OutputCodec out_codec_{OUT_ES8311};
  uint8_t out_addr_{0x18}, in_addr_{0x40};
  float mic_gain_db_{37.5f};
  uint8_t mic_channels_{0x01};  // ES7210 mic bitmask: MIC1=1 MIC2=2 MIC3=4 MIC4=8
  int out_volume_{70};
  bool use_mclk_{true};
  bool aec_enabled_{true};

  // I2S
  i2s_chan_handle_t tx_chan_{nullptr};
  i2s_chan_handle_t rx_chan_{nullptr};
  bool i2s_ready_{false};

  // codec devices (opaque esp_codec_dev handles; void* to keep header light)
  void *out_dev_{nullptr};
  void *in_dev_{nullptr};
  void *ctrl_if_{nullptr};
  void *data_if_{nullptr};
  void *gpio_if_{nullptr};
  void *out_codec_if_{nullptr};
  void *in_codec_if_{nullptr};

  // AEC (esp-sr)
  void *aec_handle_{nullptr};
  int aec_chunk_{0};
  int16_t *aec_in_{nullptr};
  int16_t *aec_ref_{nullptr};
  int16_t *aec_out_{nullptr};
  std::vector<int16_t> ref_ring_;   // far-end (speaker) reference history
  size_t ref_head_{0};
  size_t ref_count_{0};
  bool aec_ready_{false};

  bool running_{false};
  int consumers_{0};
};

}  // namespace fdaudio
}  // namespace esphome
