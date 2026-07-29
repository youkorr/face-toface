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

/// Which chip digitises the microphone.
enum MicSource : uint8_t {
  MIC_FROM_ES7210 = 0,        ///< a dedicated ES7210 ADC, the default
  MIC_FROM_OUTPUT_CODEC = 1,  ///< the ES8311/ES8388's own ADC, no ES7210 needed
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
  /// Where the microphone is captured.
  ///
  /// Not every board has an ES7210. Several ESP32-P4 boards -- the Waveshare
  /// ESP32-P4-NANO among them -- carry only the ES8311 and route the analog
  /// microphone into that codec's own ADC. On those, `MIC_FROM_OUTPUT_CODEC`
  /// opens the output codec in duplex instead of looking for a chip that is not
  /// on the bus.
  void set_mic_source(MicSource s) { mic_source_ = s; }
  void set_mic_digital_gain(float g) { mic_digital_gain_ = g; }
  void set_noise_gate(int t) { noise_gate_thresh_ = t; }
  // Far-end ducking (echo suppression for calls): when the speaker is playing
  // the far end, attenuate the mic by this % so its echo isn't sent back.
  // 0 = off. Can be changed at runtime (e.g. face2face enables it per-call so
  // it doesn't break voice_assistant barge-in).
  void set_echo_suppression(int percent) { echo_suppress_ = percent; }
  // Automatic gain control: auto-boost a weak mic toward this target peak level
  // (0 = off). Replaces guessing mic_digital_gain by hand. ~8000-12000 is a good
  // call level. This is what the board's working AFE did ("relève la voix faible").
  void set_mic_agc(int target) { mic_agc_target_ = target; }
  void set_out_volume(int v);  // 0..100, applied live to the codec if open
  void set_use_mclk(bool u) { use_mclk_ = u; }
  void set_aec_enabled(bool e) { aec_enabled_ = e; }
  // AEC gating window (ms): the AEC only adapts while the speaker has played the
  // far end within this window. During silence the adaptive filter is frozen so
  // it can't drift and start cancelling real speech. 0 = gating off (always on).
  void set_aec_gate_ms(int ms) { aec_gate_ms_ = ms; }
  // Use the full esp-sr AFE (AEC+NS+AGC with an aligned reference) instead of
  // the simple aec_create path. This is the proper echo fix.
  void set_use_afe(bool e) { use_afe_ = e; }

  // ---- Engine control (ref-counted: started while any consumer is active) ----
  bool engine_start();
  void engine_stop();
  bool engine_running() const { return running_; }
  uint32_t codec_sample_rate() const { return codec_rate_; }  // actual I2S/codec clock

  // ---- Mic read (16-bit mono at mic_rate_) / speaker write (at codec_rate_) ----
  // Returns bytes actually read/written. Mic data is AEC-cleaned if enabled.
  size_t read_mic(uint8_t *dst, size_t len);
  void write_speaker(const uint8_t *src, size_t len);

 protected:
  bool init_i2s_();
  bool init_codecs_();
  bool init_aec_();
  void deinit_();
  /// Release the I2S channels. A channel pair is a finite hardware resource:
  /// leak one and every later i2s_new_channel() on that port returns
  /// ESP_ERR_NOT_FOUND, so the original fault hides behind a wrong error for the
  /// rest of the boot. Safe to call at any point of a partial init.
  void free_i2s_();
  /// Release everything init_codecs_() created, in reverse order. Called on the
  /// failure path too, because the microphone retries from loop(): without it,
  /// every attempt would allocate another set of interfaces.
  void free_codecs_();
  void run_aec_(int16_t *mic, size_t samples);  // in-place on mic buffer
  // True while the far end (speaker) has played recently enough that its echo is
  // still in the mic and the AEC/AFE should adapt; false during silence so the
  // adaptive filter is frozen. Shared by the simple AEC and the AFE feed.
  bool far_end_active_() const;

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
  // Far-end ducking / echo suppression (0 = off, else % attenuation).
  int echo_suppress_{0};
  float duck_gain_{1.0f};
  uint32_t last_spk_ms_{0};
  int32_t last_spk_peak_{0};
  /// Last time the far end was genuinely loud (peak > 800), as opposed to any
  /// write at all. The ducking hangover counts from this.
  uint32_t last_loud_spk_ms_{0};
  /// How long the microphone stays ducked after the far end goes quiet. Must
  /// outlast the gaps inside speech (~50-200 ms) or the duck lifts between two
  /// words and lets the room's echo of the first one through; 400 ms also
  /// covers a normal room's reverberation without swallowing a reply.
  static constexpr uint32_t ECHO_HOLD_MS = 400;
  // Automatic gain control (0 = off, else target peak level).
  int mic_agc_target_{0};
  float agc_env_{0.0f};
  float agc_gain_{1.0f};
  int i2c_port_{0};
  OutputCodec out_codec_{OUT_ES8311};
  MicSource mic_source_{MIC_FROM_ES7210};
  /// True when one duplex device serves both directions, so the teardown does
  /// not close and delete the same handle twice.
  bool shared_dev_{false};
  uint8_t out_addr_{0x18}, in_addr_{0x40};
  float mic_gain_db_{37.5f};
  uint8_t mic_channels_{0x01};  // ES7210 mic bitmask: MIC1=1 MIC2=2 MIC3=4 MIC4=8
  int out_volume_{70};
  bool use_mclk_{true};
  bool aec_enabled_{true};
  int aec_gate_ms_{250};  // AEC adaptation window after speaker activity (0 = off)

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
  // The two I2C control interfaces. Members rather than locals so the teardown
  // can actually delete them.
  void *out_ctrl_if_{nullptr};
  void *in_ctrl_if_{nullptr};

  // Failed-start backoff. engine_start() is called from the microphone's loop(),
  // so without a brake a failing init runs thousands of times a minute.
  static constexpr uint32_t START_RETRY_MS = 1000;
  bool start_failed_{false};
  uint32_t last_start_fail_ms_{0};

  // AEC (esp-sr simple aec_create path)
  void *aec_handle_{nullptr};
  int aec_chunk_{0};
  int16_t *aec_in_{nullptr};
  int16_t *aec_ref_{nullptr};
  int16_t *aec_out_{nullptr};
  std::vector<int16_t> ref_ring_;   // far-end (speaker) reference history
  size_t ref_head_{0};
  size_t ref_count_{0};
  bool aec_ready_{false};

  // Full AFE (esp-sr) path: AEC + NS + AGC with a time-aligned reference.
  bool use_afe_{false};
  bool afe_active_{false};
  void *afe_handle_{nullptr};   // esp_afe_sr_iface_t*
  void *afe_data_{nullptr};     // esp_afe_sr_data_t*
  int afe_chunk_{0};            // feed samples per channel
  int afe_nch_{0};              // feed channels (3 for "MNR")
  std::vector<int16_t> afe_feed_;    // interleaved [M,N,R] feed buffer
  std::vector<int16_t> afe_micbuf_;  // decimated mono mic chunk

  bool init_afe_();
  size_t read_mic_afe_(uint8_t *dst, size_t len);
  // The AFE needs feed() and fetch() on separate tasks: this task continuously
  // reads the codec mic and feeds the AFE; read_mic_afe_ only fetches.
  static void afe_feed_task_(void *param);
  void *afe_feed_handle_{nullptr};  // TaskHandle_t (kept as void* in header)
  volatile bool afe_feed_run_{false};

  bool running_{false};
  int consumers_{0};
};

}  // namespace fdaudio
}  // namespace esphome
