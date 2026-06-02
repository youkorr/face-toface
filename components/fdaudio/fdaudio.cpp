#include "fdaudio.h"
#include "esphome/core/log.h"

#include <cstring>

// ESP-IDF new I2C master driver (to reuse ESPHome's bus by port).
#include "driver/i2c_master.h"

// esp_codec_dev (Espressif managed component, pulled in __init__.py)
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"

// esp-sr AEC (pulled when aec enabled)
#ifdef FDAUDIO_USE_AEC
#include "esp_aec.h"
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace fdaudio {

static const char *const TAG = "fdaudio";

void FdAudio::setup() {
  ESP_LOGCONFIG(TAG, "Setting up fdaudio (full-duplex I2S, Espressif-native)...");
  // I2S + codecs are created lazily on engine_start() so the bus is only
  // powered while audio is actually used.
}

void FdAudio::dump_config() {
  ESP_LOGCONFIG(TAG, "fdaudio:");
  ESP_LOGCONFIG(TAG, "  I2S: mclk=%d bclk=%d lrclk=%d din=%d dout=%d", mclk_pin_, bclk_pin_,
                lrclk_pin_, din_pin_, dout_pin_);
  ESP_LOGCONFIG(TAG, "  Sample rate: %u Hz", sample_rate_);
  ESP_LOGCONFIG(TAG, "  Output codec: %s (0x%02X), mic ES7210 (0x%02X)",
                out_codec_ == OUT_ES8311 ? "ES8311" : "ES8388", out_addr_, in_addr_);
  ESP_LOGCONFIG(TAG, "  AEC: %s", aec_enabled_ ? "enabled" : "off");
}

// ===========================================================================
// I2S full-duplex: one port, TX + RX handles (shared clock)
// ===========================================================================
bool FdAudio::init_i2s_() {
  if (i2s_ready_)
    return true;

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  // Both handles non-NULL => full-duplex on the same port.
  if (i2s_new_channel(&chan_cfg, &tx_chan_, &rx_chan_) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel (duplex) failed");
    return false;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = (gpio_num_t) mclk_pin_,
          .bclk = (gpio_num_t) bclk_pin_,
          .ws = (gpio_num_t) lrclk_pin_,
          .dout = (gpio_num_t) dout_pin_,
          .din = (gpio_num_t) din_pin_,
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  if (i2s_channel_init_std_mode(tx_chan_, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "i2s tx init_std failed");
    return false;
  }
  if (i2s_channel_init_std_mode(rx_chan_, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "i2s rx init_std failed");
    return false;
  }
  if (i2s_channel_enable(tx_chan_) != ESP_OK || i2s_channel_enable(rx_chan_) != ESP_OK) {
    ESP_LOGE(TAG, "i2s channel enable failed");
    return false;
  }
  i2s_ready_ = true;
  ESP_LOGI(TAG, "I2S full-duplex ready (TX+RX on port 0)");
  return true;
}

// ===========================================================================
// Codecs: ES8311/ES8388 (OUT) + ES7210 (IN) sharing the same I2S data interface
// ===========================================================================
bool FdAudio::init_codecs_() {
  if (out_dev_ != nullptr)
    return true;

  // Shared data interface (the duplex I2S handles).
  audio_codec_i2s_cfg_t i2s_data_cfg = {};
  i2s_data_cfg.port = I2S_NUM_0;
  i2s_data_cfg.rx_handle = rx_chan_;
  i2s_data_cfg.tx_handle = tx_chan_;
  data_if_ = (void *) audio_codec_new_i2s_data(&i2s_data_cfg);

  gpio_if_ = (void *) audio_codec_new_gpio();

  // Reuse the I2C bus ESPHome already created on this port, instead of letting
  // esp_codec_dev create a second one (which aborts: "driver_ng is not allowed
  // to be used with this old driver" / bus conflict).
  i2c_master_bus_handle_t i2c_bus = nullptr;
  if (i2c_master_get_bus_handle((i2c_port_t) i2c_port_, &i2c_bus) != ESP_OK || i2c_bus == nullptr) {
    ESP_LOGE(TAG, "could not get ESPHome I2C bus handle on port %d", i2c_port_);
    return false;
  }

  // esp_codec_dev expects the I2C address in 8-bit form (7-bit << 1), while
  // YAML/ESPHome uses the standard 7-bit address (e.g. ES8311 0x18, ES7210
  // 0x40). Shift left by 1 so "Fail to write to dev" goes away.
  uint8_t out_addr8 = (uint8_t) (out_addr_ << 1);
  uint8_t in_addr8 = (uint8_t) (in_addr_ << 1);

  // --- Output codec control (I2C) ---
  audio_codec_i2c_cfg_t out_i2c = {};
  out_i2c.port = (uint8_t) i2c_port_;
  out_i2c.addr = out_addr8;
  out_i2c.bus_handle = i2c_bus;
  const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&out_i2c);

  if (out_codec_ == OUT_ES8311) {
    es8311_codec_cfg_t cfg = {};
    cfg.ctrl_if = out_ctrl;
    cfg.gpio_if = (const audio_codec_gpio_if_t *) gpio_if_;
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    cfg.use_mclk = use_mclk_;
    cfg.pa_pin = -1;
    out_codec_if_ = (void *) es8311_codec_new(&cfg);
  } else {  // ES8388
    es8388_codec_cfg_t cfg = {};
    cfg.ctrl_if = out_ctrl;
    cfg.gpio_if = (const audio_codec_gpio_if_t *) gpio_if_;
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    cfg.master_mode = false;
    cfg.pa_pin = -1;
    out_codec_if_ = (void *) es8388_codec_new(&cfg);
  }
  if (out_codec_if_ == nullptr) {
    ESP_LOGE(TAG, "output codec new failed");
    return false;
  }

  esp_codec_dev_cfg_t out_dev_cfg = {};
  out_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  out_dev_cfg.codec_if = (const audio_codec_if_t *) out_codec_if_;
  out_dev_cfg.data_if = (const audio_codec_data_if_t *) data_if_;
  out_dev_ = esp_codec_dev_new(&out_dev_cfg);

  // --- Mic codec ES7210 (I2C) ---
  audio_codec_i2c_cfg_t in_i2c = {};
  in_i2c.port = (uint8_t) i2c_port_;
  in_i2c.addr = in_addr8;
  in_i2c.bus_handle = i2c_bus;
  const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&in_i2c);

  es7210_codec_cfg_t mic_cfg = {};
  mic_cfg.ctrl_if = in_ctrl;
  mic_cfg.mic_selected = ES7210_SEL_MIC1;  // single mic on channel 1
  in_codec_if_ = (void *) es7210_codec_new(&mic_cfg);
  if (in_codec_if_ == nullptr) {
    ESP_LOGE(TAG, "ES7210 codec new failed");
    return false;
  }

  esp_codec_dev_cfg_t in_dev_cfg = {};
  in_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  in_dev_cfg.codec_if = (const audio_codec_if_t *) in_codec_if_;
  in_dev_cfg.data_if = (const audio_codec_data_if_t *) data_if_;
  in_dev_ = esp_codec_dev_new(&in_dev_cfg);

  if (out_dev_ == nullptr || in_dev_ == nullptr) {
    ESP_LOGE(TAG, "esp_codec_dev_new failed");
    return false;
  }

  // Open both at the same PCM format (16-bit mono).
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 1;
  fs.sample_rate = sample_rate_;
  esp_codec_dev_open((esp_codec_dev_handle_t) out_dev_, &fs);
  esp_codec_dev_open((esp_codec_dev_handle_t) in_dev_, &fs);
  esp_codec_dev_set_out_vol((esp_codec_dev_handle_t) out_dev_, out_volume_);
  esp_codec_dev_set_in_gain((esp_codec_dev_handle_t) in_dev_, mic_gain_db_);

  ESP_LOGI(TAG, "Codecs ready (%s out + ES7210 in)", out_codec_ == OUT_ES8311 ? "ES8311" : "ES8388");
  return true;
}

// ===========================================================================
// AEC (esp-sr) — software echo reference from the last played speaker frame
// ===========================================================================
bool FdAudio::init_aec_() {
#ifdef FDAUDIO_USE_AEC
  if (!aec_enabled_ || aec_ready_)
    return aec_ready_;
  aec_handle_ = aec_create((int) sample_rate_, 4, 1, AEC_MODE_SR_LOW_COST);
  if (aec_handle_ == nullptr) {
    ESP_LOGW(TAG, "aec_create failed; mic will be raw");
    return false;
  }
  aec_chunk_ = aec_get_chunksize(static_cast<aec_handle_t *>(aec_handle_));
  if (aec_chunk_ <= 0)
    return false;
  size_t bytes = (size_t) aec_chunk_ * sizeof(int16_t);
  aec_in_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DEFAULT));
  aec_ref_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DEFAULT));
  aec_out_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DEFAULT));
  ref_ring_.assign((size_t) aec_chunk_ * 32, 0);
  ref_head_ = 0;
  ref_count_ = 0;
  aec_ready_ = (aec_in_ && aec_ref_ && aec_out_);
  if (aec_ready_)
    ESP_LOGI(TAG, "AEC ready (chunk=%d)", aec_chunk_);
  return aec_ready_;
#else
  return false;
#endif
}

void FdAudio::run_aec_(int16_t *mic, size_t samples) {
#ifdef FDAUDIO_USE_AEC
  if (!aec_ready_)
    return;
  // Process in chunk_-sized blocks; pull the time-aligned far-end reference.
  size_t off = 0;
  while (off + (size_t) aec_chunk_ <= samples) {
    std::memcpy(aec_in_, mic + off, (size_t) aec_chunk_ * sizeof(int16_t));
    for (int i = 0; i < aec_chunk_; i++) {
      if (ref_count_ > 0) {
        aec_ref_[i] = ref_ring_[ref_head_];
        ref_head_ = (ref_head_ + 1) % ref_ring_.size();
        ref_count_--;
      } else {
        aec_ref_[i] = 0;
      }
    }
    aec_process(static_cast<aec_handle_t *>(aec_handle_), aec_in_, aec_ref_, aec_out_);
    std::memcpy(mic + off, aec_out_, (size_t) aec_chunk_ * sizeof(int16_t));
    off += aec_chunk_;
  }
#else
  (void) mic; (void) samples;
#endif
}

// ===========================================================================
// Engine lifecycle (ref-counted)
// ===========================================================================
bool FdAudio::engine_start() {
  consumers_++;
  if (running_)
    return true;
  if (!init_i2s_() || !init_codecs_()) {
    ESP_LOGE(TAG, "engine_start failed");
    return false;
  }
  if (aec_enabled_)
    init_aec_();
  running_ = true;
  ESP_LOGI(TAG, "Audio engine started (consumers=%d)", consumers_);
  return true;
}

void FdAudio::engine_stop() {
  if (consumers_ > 0)
    consumers_--;
  if (consumers_ > 0 || !running_)
    return;
  // Keep codecs/I2S allocated (cheap to keep, costly to re-init); just mark idle.
  running_ = false;
  ESP_LOGI(TAG, "Audio engine idle");
}

// ===========================================================================
// PCM I/O
// ===========================================================================
size_t FdAudio::read_mic(uint8_t *dst, size_t len) {
  if (in_dev_ == nullptr)
    return 0;
  // esp_codec_dev_read returns a STATUS code (ESP_CODEC_DEV_OK == 0 on success),
  // NOT a byte count. On success it has filled the whole `len` buffer (blocking).
  int ret = esp_codec_dev_read((esp_codec_dev_handle_t) in_dev_, dst, (int) len);
  if (ret != ESP_CODEC_DEV_OK)
    return 0;
  size_t got = len;
  if (aec_enabled_)
    run_aec_(reinterpret_cast<int16_t *>(dst), got / 2);

  // Throttled mic level meter: tells us whether the codec returns real audio
  // (peak moves when you talk) or constant silence (codec/wiring issue).
  static uint32_t dbg = 0;
  if ((dbg++ & 0x1F) == 0) {  // ~ every 32 reads
    const int16_t *s = reinterpret_cast<const int16_t *>(dst);
    size_t n = got / 2;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
      int32_t a = s[i] < 0 ? -s[i] : s[i];
      if (a > peak)
        peak = a;
    }
    ESP_LOGD(TAG, "mic level: peak=%d (%u bytes)", (int) peak, (unsigned) got);
  }
  return got;
}

void FdAudio::write_speaker(const uint8_t *src, size_t len) {
  if (out_dev_ == nullptr || len == 0)
    return;
#ifdef FDAUDIO_USE_AEC
  // Store the far-end frame we are about to play as the echo reference.
  if (aec_ready_) {
    const int16_t *s = reinterpret_cast<const int16_t *>(src);
    size_t n = len / 2;
    for (size_t i = 0; i < n; i++) {
      size_t t = (ref_head_ + ref_count_) % ref_ring_.size();
      ref_ring_[t] = s[i];
      if (ref_count_ < ref_ring_.size())
        ref_count_++;
      else
        ref_head_ = (ref_head_ + 1) % ref_ring_.size();
    }
  }
#endif
  esp_codec_dev_write((esp_codec_dev_handle_t) out_dev_, (void *) src, (int) len);
}

void FdAudio::deinit_() {
  if (out_dev_) { esp_codec_dev_close((esp_codec_dev_handle_t) out_dev_); esp_codec_dev_delete((esp_codec_dev_handle_t) out_dev_); out_dev_ = nullptr; }
  if (in_dev_) { esp_codec_dev_close((esp_codec_dev_handle_t) in_dev_); esp_codec_dev_delete((esp_codec_dev_handle_t) in_dev_); in_dev_ = nullptr; }
  if (tx_chan_) { i2s_channel_disable(tx_chan_); }
  if (rx_chan_) { i2s_channel_disable(rx_chan_); }
  i2s_ready_ = false;
}

}  // namespace fdaudio
}  // namespace esphome
