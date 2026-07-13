#include "fdaudio.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // millis()

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
// Full AFE (AEC + NS + AGC with an aligned reference).
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
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
  ESP_LOGCONFIG(TAG, "  Codec/I2S rate: %u Hz, mic output: %u Hz (decim %u:1)", codec_rate_,
                mic_rate_, codec_rate_ / mic_rate_);
  ESP_LOGCONFIG(TAG, "  Mic digital gain: x%.2f", mic_digital_gain_);
  ESP_LOGCONFIG(TAG, "  Noise gate: %s (thresh=%d)", noise_gate_thresh_ > 0 ? "on" : "off",
                noise_gate_thresh_);
  ESP_LOGCONFIG(TAG, "  Output codec: %s (0x%02X), mic ES7210 (0x%02X)",
                out_codec_ == OUT_ES8311 ? "ES8311" : "ES8388", out_addr_, in_addr_);
  ESP_LOGCONFIG(TAG, "  AEC: %s", aec_enabled_ ? "enabled" : "off");
}

void FdAudio::set_out_volume(int v) {
  if (v < 0)
    v = 0;
  if (v > 100)
    v = 100;
  out_volume_ = v;
  // Apply immediately if the output codec is already open (live volume from HA).
  if (out_dev_ != nullptr)
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t) out_dev_, out_volume_);
}

// ===========================================================================
// I2S full-duplex: one port, TX + RX handles (shared clock)
// ===========================================================================
bool FdAudio::init_i2s_() {
  if (i2s_ready_)
    return true;

  // If a previous attempt failed part-way (e.g. DMA buffer allocation failed
  // because internal SRAM was momentarily exhausted), the channel handles must
  // be RELEASED before retrying: they stay registered on the controller, and
  // the next i2s_new_channel() would fail forever with "no available channel
  // found" even once memory is back.
  auto release_channels = [this]() {
    if (tx_chan_ != nullptr) {
      i2s_channel_disable(tx_chan_);  // no-op/error if not enabled; ignore
      i2s_del_channel(tx_chan_);
      tx_chan_ = nullptr;
    }
    if (rx_chan_ != nullptr) {
      i2s_channel_disable(rx_chan_);
      i2s_del_channel(rx_chan_);
      rx_chan_ = nullptr;
    }
  };
  release_channels();  // clean slate if an old partial attempt left handles

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  // Both handles non-NULL => full-duplex on the same port.
  if (i2s_new_channel(&chan_cfg, &tx_chan_, &rx_chan_) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel (duplex) failed");
    tx_chan_ = nullptr;
    rx_chan_ = nullptr;
    return false;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(codec_rate_),
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
    ESP_LOGE(TAG, "i2s tx init_std failed (freeing channels for retry)");
    release_channels();
    return false;
  }
  if (i2s_channel_init_std_mode(rx_chan_, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "i2s rx init_std failed (freeing channels for retry)");
    release_channels();
    return false;
  }
  if (i2s_channel_enable(tx_chan_) != ESP_OK || i2s_channel_enable(rx_chan_) != ESP_OK) {
    ESP_LOGE(TAG, "i2s channel enable failed (freeing channels for retry)");
    release_channels();
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
  // Which physical mic input(s) of the ES7210 are wired to the board's mic.
  // Configurable from YAML because it differs per board (often MIC1, but some
  // route the analog mic to MIC2/3/4). Bitmask: MIC1=1 MIC2=2 MIC3=4 MIC4=8.
  // The enum type name of 'mic_selected' differs between esp_codec_dev
  // versions (es7210_input_mics_t / es7210_mic_select_t / ...), so cast to the
  // field's own type via decltype to stay version-agnostic.
  mic_cfg.mic_selected = (decltype(mic_cfg.mic_selected)) mic_channels_;
  ESP_LOGI(TAG, "ES7210 mic_selected bitmask=0x%02X", mic_channels_);
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

  // Open both at the same PCM format (16-bit mono) at the CODEC clock rate.
  // The mic is later decimated to mic_rate_ for ESPHome; the speaker is fed at
  // codec_rate_ directly. This matches the board's known-working 48 kHz setup.
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 1;
  fs.sample_rate = codec_rate_;
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
  // AEC runs on the decimated mic output (mic_rate_, e.g. 16 kHz).
  aec_handle_ = aec_create((int) mic_rate_, 4, 1, AEC_MODE_SR_LOW_COST);
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
  // Keep the reference SHORT (~2 frames) so it stays time-aligned with the echo
  // (like 'previous_frame'): a long FIFO let the reference lag by up to ~0.5 s,
  // which the AEC can't align -> echo leaks. 2 chunks (~32 ms) matches the
  // acoustic+codec delay and is well within filter_length.
  ref_ring_.assign((size_t) aec_chunk_ * 2, 0);
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
// Full AFE (esp-sr): AEC + NS + AGC. Input format "MNR" = [Mic, Null, Reference]
// at mic_rate_. The reference is the far-end (speaker) frame, time-aligned by
// the AFE internally -- this is what makes the echo actually cancel.
// ===========================================================================
bool FdAudio::init_afe_() {
#ifdef FDAUDIO_USE_AEC
  if (afe_active_)
    return true;
  afe_config_t *cfg = afe_config_init("MNR", nullptr, AFE_TYPE_VC, AFE_MODE_LOW_COST);
  if (cfg == nullptr) {
    ESP_LOGW(TAG, "afe_config_init failed");
    return false;
  }
  cfg->aec_init = true;
  cfg->ns_init = true;
  cfg->agc_init = true;
  cfg->se_init = false;       // no BSS/speech-enhancement (single mic)
  cfg->vad_init = false;
  cfg->wakenet_init = false;  // no wake word in the AFE (mWW stays separate)
  cfg->pcm_config.sample_rate = (int) mic_rate_;

  const esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(cfg);
  if (afe == nullptr) {
    ESP_LOGW(TAG, "esp_afe_handle_from_config failed");
    afe_config_free(cfg);
    return false;
  }
  esp_afe_sr_data_t *data = afe->create_from_config(cfg);
  afe_config_free(cfg);
  if (data == nullptr) {
    ESP_LOGW(TAG, "AFE create_from_config failed");
    return false;
  }
  afe_handle_ = (void *) afe;
  afe_data_ = (void *) data;
  afe_chunk_ = afe->get_feed_chunksize(data);
  afe_nch_ = afe->get_feed_channel_num(data);
  if (afe_chunk_ <= 0 || afe_nch_ <= 0) {
    ESP_LOGW(TAG, "AFE bad chunk/nch (%d/%d)", afe_chunk_, afe_nch_);
    return false;
  }
  afe_feed_.assign((size_t) afe_chunk_ * afe_nch_, 0);
  afe_micbuf_.assign((size_t) afe_chunk_, 0);
  // Reference history (far-end), sized generously for the AFE's delay search.
  ref_ring_.assign((size_t) afe_chunk_ * 64, 0);
  ref_head_ = 0;
  ref_count_ = 0;
  afe_active_ = true;
  // The AFE needs feed() and fetch() on separate tasks (it has an internal
  // processing thread). Start the dedicated feed task; read_mic_afe_ fetches.
  afe_feed_run_ = true;
  TaskHandle_t t = nullptr;
  // 8 KB stack: the AFE feed() runs WebRTC NS/AGC DSP and overflows a 4 KB stack.
  xTaskCreatePinnedToCore(afe_feed_task_, "fdaudio_afe_feed", 8192, this, 5, &t, 1);
  afe_feed_handle_ = (void *) t;
  ESP_LOGI(TAG, "AFE ready (AEC+NS+AGC, chunk=%d, channels=%d, fmt=MNR)", afe_chunk_, afe_nch_);
  return true;
#else
  return false;
#endif
}

#ifdef FDAUDIO_USE_AEC
// Continuously read the codec mic, decimate to mic_rate_, interleave [M,N,R]
// (ref = far-end from write_speaker), and feed the AFE. Runs in its own task so
// the AFE's internal processing always has input (fixes "Ringbuffer is empty").
void FdAudio::afe_feed_task_(void *param) {
  auto *self = static_cast<FdAudio *>(param);
  auto *afe = static_cast<const esp_afe_sr_iface_t *>(self->afe_handle_);
  auto *data = static_cast<esp_afe_sr_data_t *>(self->afe_data_);
  uint32_t decim = self->codec_rate_ / self->mic_rate_;
  if (decim < 1)
    decim = 1;
  const int chunk = self->afe_chunk_;
  const int nch = self->afe_nch_;
  std::vector<int16_t> scratch((size_t) chunk * decim);

  while (self->afe_feed_run_) {
    if (self->in_dev_ == nullptr) {
      vTaskDelay(1);
      continue;
    }
    if (esp_codec_dev_read((esp_codec_dev_handle_t) self->in_dev_, scratch.data(),
                           (int) (scratch.size() * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
      vTaskDelay(1);
      continue;
    }
    for (int i = 0; i < chunk; i++) {
      int32_t acc = 0;
      for (uint32_t k = 0; k < decim; k++)
        acc += scratch[(size_t) i * decim + k];
      int32_t s = (int32_t) ((acc / (int32_t) decim) * self->mic_digital_gain_);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      int16_t ref = 0;
      if (self->ref_count_ > 0) {
        ref = self->ref_ring_[self->ref_head_];
        self->ref_head_ = (self->ref_head_ + 1) % self->ref_ring_.size();
        self->ref_count_--;
      }
      int base = i * nch;
      self->afe_feed_[base + 0] = (int16_t) s;            // M (mic)
      for (int c = 1; c < nch - 1; c++)
        self->afe_feed_[base + c] = 0;                    // N (null)
      self->afe_feed_[base + (nch - 1)] = ref;            // R (reference)
    }
    afe->feed(data, self->afe_feed_.data());
  }
  self->afe_feed_handle_ = nullptr;
  vTaskDelete(nullptr);
}
#endif

size_t FdAudio::read_mic_afe_(uint8_t *dst, size_t len) {
#ifdef FDAUDIO_USE_AEC
  auto *afe = static_cast<const esp_afe_sr_iface_t *>(afe_handle_);
  auto *data = static_cast<esp_afe_sr_data_t *>(afe_data_);
  // The feed task supplies the AFE; here we only pull the cleaned mono output.
  afe_fetch_result_t *res = afe->fetch(data);
  if (res == nullptr || res->data == nullptr || res->data_size <= 0)
    return 0;
  size_t n = (size_t) res->data_size;
  if (n > len)
    n = len;
  std::memcpy(dst, res->data, n);
  return n;
#else
  (void) dst; (void) len;
  return 0;
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
    // Roll the refcount back so a later retry (e.g. micro_wake_word restarting
    // the mic) starts from a balanced count and engine_stop can still idle.
    consumers_--;
    ESP_LOGE(TAG, "engine_start failed");
    return false;
  }
  if (use_afe_) {
    // Full AFE (AEC+NS+AGC); fall back to the simple AEC if it fails to init.
    if (!init_afe_() && aec_enabled_)
      init_aec_();
  } else if (aec_enabled_) {
    init_aec_();
  }
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

  // Full AFE path (AEC+NS+AGC): returns the cleaned mic directly. Skip the
  // simple AEC / noise gate / AGC / ducking below (the AFE already does it).
  if (afe_active_)
    return read_mic_afe_(dst, len);

  // The codec runs at codec_rate_ (e.g. 48 kHz) but ESPHome wants mic_rate_
  // (e.g. 16 kHz). Read decim x more samples and average each group down.
  uint32_t decim = codec_rate_ / mic_rate_;
  if (decim < 1)
    decim = 1;
  size_t out_samples = len / 2;               // requested 16 kHz samples
  size_t in_samples = out_samples * decim;    // codec-rate samples to read
  if (mic_scratch_.size() < in_samples)
    mic_scratch_.resize(in_samples);

  // esp_codec_dev_read returns a STATUS code (ESP_CODEC_DEV_OK == 0 on success),
  // NOT a byte count. On success it has filled the whole buffer (blocking).
  int ret = esp_codec_dev_read((esp_codec_dev_handle_t) in_dev_, mic_scratch_.data(),
                               (int) (in_samples * sizeof(int16_t)));

  int16_t *out = reinterpret_cast<int16_t *>(dst);
  int32_t raw_peak = 0;
  if (ret == ESP_CODEC_DEV_OK) {
    for (size_t j = 0; j < in_samples; j++) {
      int32_t a = mic_scratch_[j] < 0 ? -mic_scratch_[j] : mic_scratch_[j];
      if (a > raw_peak)
        raw_peak = a;
    }
  }

  // Loud, unmissable diagnostic for the first reads (INFO so it shows without
  // DEBUG): confirms read_mic runs, the codec read status, and the RAW codec
  // level BEFORE any decimation/gain. raw_peak ~0 => codec gives silence.
  static uint32_t first = 0;
  if (first < 10) {
    first++;
    ESP_LOGI(TAG, "mic read #%u: ret=%d raw_peak=%d (read %u codec samples)", (unsigned) first,
             ret, (int) raw_peak, (unsigned) in_samples);
  }
  if (ret != ESP_CODEC_DEV_OK)
    return 0;

  for (size_t i = 0; i < out_samples; i++) {
    int32_t acc = 0;
    for (uint32_t k = 0; k < decim; k++)
      acc += mic_scratch_[i * decim + k];
    int32_t s = (int32_t) ((acc / (int32_t) decim) * mic_digital_gain_);  // decimate + boost
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out[i] = (int16_t) s;
  }
  size_t got = out_samples * sizeof(int16_t);

  if (aec_enabled_)
    run_aec_(out, got / 2);

  // Lightweight noise gate: pass speech (envelope above threshold), attenuate
  // ambient noise. Instant attack, ~50 ms envelope release; gain opens in ~5 ms
  // and closes in ~150 ms so there are no clicks and the start of "ok nabu"
  // isn't clipped. 0 = disabled (default).
  if (noise_gate_thresh_ > 0) {
    const float thr = (float) noise_gate_thresh_;
    for (size_t i = 0; i < got / 2; i++) {
      float a = out[i] < 0 ? -(float) out[i] : (float) out[i];
      gate_env_ = a > gate_env_ ? a : gate_env_ * 0.99913f;  // attack / release
      const float target = gate_env_ > thr ? 1.0f : 0.0f;
      if (target > gate_gain_) {
        gate_gain_ += 0.0125f;  // open ~5 ms
        if (gate_gain_ > 1.0f)
          gate_gain_ = 1.0f;
      } else {
        gate_gain_ -= 0.000417f;  // close ~150 ms (acts as a hold)
        if (gate_gain_ < 0.0f)
          gate_gain_ = 0.0f;
      }
      out[i] = (int16_t) ((float) out[i] * gate_gain_);
    }
  }

  // Automatic gain control: bring a weak mic up to a target level (per frame,
  // smoothed across frames). Applied after the noise gate so silence (already
  // gated to ~0) isn't blasted, and before ducking so echo suppression still
  // wins when the far end plays. This is what fixes "call audio too faint".
  if (mic_agc_target_ > 0) {
    int32_t pk = 0;
    for (size_t i = 0; i < got / 2; i++) {
      int32_t a = out[i] < 0 ? -out[i] : out[i];
      if (a > pk)
        pk = a;
    }
    const float fpk = (float) pk;
    if (fpk > agc_env_)
      agc_env_ += (fpk - agc_env_) * 0.6f;  // attack
    else
      agc_env_ *= 0.95f;                     // release (~0.6 s)
    const float env = agc_env_ < 1.0f ? 1.0f : agc_env_;
    float desired = (float) mic_agc_target_ / env;
    const float kMaxGain = 12.0f;
    if (desired > kMaxGain)
      desired = kMaxGain;
    if (desired < 1.0f)
      desired = 1.0f;
    agc_gain_ += (desired - agc_gain_) * 0.2f;  // smooth gain across frames
    for (size_t i = 0; i < got / 2; i++) {
      int32_t v = (int32_t) ((float) out[i] * agc_gain_);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      out[i] = (int16_t) v;
    }
  }

  // Far-end ducking (echo suppression): when the speaker recently played the
  // far end at a meaningful level, attenuate the mic so we don't send its echo
  // back (the robust intercom fix when AEC alone can't cancel it). Smoothed to
  // avoid pumping: ducks fast (~3 ms), releases slower (~17 ms).
  if (echo_suppress_ > 0) {
    const bool far_active = (millis() - last_spk_ms_ < 120) && (last_spk_peak_ > 800);
    const float floor = 1.0f - (float) echo_suppress_ / 100.0f;
    for (size_t i = 0; i < got / 2; i++) {
      if (far_active)
        duck_gain_ -= 0.02f;
      else
        duck_gain_ += 0.003f;
      if (duck_gain_ < floor)
        duck_gain_ = floor;
      if (duck_gain_ > 1.0f)
        duck_gain_ = 1.0f;
      out[i] = (int16_t) ((float) out[i] * duck_gain_);
    }
  }

  return got;
}

void FdAudio::write_speaker(const uint8_t *src, size_t len) {
  if (out_dev_ == nullptr || len == 0)
    return;
  // Track speaker activity + level for the far-end ducking in read_mic.
  {
    const int16_t *s = reinterpret_cast<const int16_t *>(src);
    size_t n = len / 2;
    int32_t p = 0;
    for (size_t i = 0; i < n; i++) {
      int32_t a = s[i] < 0 ? -s[i] : s[i];
      if (a > p)
        p = a;
    }
    last_spk_peak_ = p;
    last_spk_ms_ = millis();
  }
#ifdef FDAUDIO_USE_AEC
  // Store the far-end frame as the echo reference, decimated to mic_rate_ so it
  // aligns with the (decimated) mic before AEC/AFE. Used by both paths.
  if (aec_ready_ || afe_active_) {
    const int16_t *s = reinterpret_cast<const int16_t *>(src);
    size_t n = len / 2;
    uint32_t decim = codec_rate_ / mic_rate_;
    if (decim < 1)
      decim = 1;
    for (size_t i = 0; i + decim <= n; i += decim) {
      int32_t acc = 0;
      for (uint32_t k = 0; k < decim; k++)
        acc += s[i + k];
      size_t t = (ref_head_ + ref_count_) % ref_ring_.size();
      ref_ring_[t] = (int16_t) (acc / (int32_t) decim);
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
