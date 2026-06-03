#include "fdaudio_speaker.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace fdaudio {

static const char *const TAG = "fdaudio.spk";

void FdAudioSpeaker::setup() {
  this->ring_ = ring_buffer::RingBuffer::create(16384);
}

void FdAudioSpeaker::dump_config() { ESP_LOGCONFIG(TAG, "fdaudio speaker"); }

void FdAudioSpeaker::loop() {}

void FdAudioSpeaker::start() {
  if (this->state_ == speaker::STATE_RUNNING || this->want_run_)
    return;
  if (parent_ == nullptr || !parent_->engine_start()) {
    ESP_LOGE(TAG, "engine start failed");
    return;
  }
  this->want_run_ = true;
  this->state_ = speaker::STATE_STARTING;
  xTaskCreatePinnedToCore(write_task_, "fdaudio_spk", 4096, this, 5, &this->task_, 1);
}

void FdAudioSpeaker::stop() {
  if (!this->want_run_)
    return;
  this->want_run_ = false;
  this->state_ = speaker::STATE_STOPPING;
}

size_t FdAudioSpeaker::play(const uint8_t *data, size_t length) {
  if (this->ring_ == nullptr)
    return 0;
  if (this->state_ != speaker::STATE_RUNNING && !this->want_run_)
    this->start();
  // Capture the caller's sample rate so write_task_ can upsample to the codec
  // rate. Data is queued at its source rate (the return value still reflects the
  // caller's byte count) and resampled on the way out.
  this->src_rate_ = this->audio_stream_info_.get_sample_rate();
  return this->ring_->write_without_replacement(data, length, 0);
}

bool FdAudioSpeaker::has_buffered_data() const {
  return this->ring_ != nullptr && this->ring_->available() > 0;
}

void FdAudioSpeaker::write_task_(void *param) {
  auto *self = static_cast<FdAudioSpeaker *>(param);
  self->state_ = speaker::STATE_RUNNING;
  std::vector<uint8_t> buf(1024);
  while (self->want_run_) {
    size_t got = self->ring_->read(buf.data(), buf.size(), 10 / portTICK_PERIOD_MS);
    if (got == 0) {
      vTaskDelay(1);
      continue;
    }
    // Apply the media_player / speaker software volume + mute. The base
    // speaker::set_volume() only stores volume_ (it forwards to an audio_dac we
    // don't have), so without this the media_player volume slider does nothing.
    const float vol = self->get_mute_state() ? 0.0f : self->get_volume();
    if (vol <= 0.001f) {
      std::memset(buf.data(), 0, got);
    } else if (vol < 0.999f) {
      int16_t *s = reinterpret_cast<int16_t *>(buf.data());
      const size_t n = got / sizeof(int16_t);
      for (size_t i = 0; i < n; i++)
        s[i] = (int16_t) ((float) s[i] * vol);
    }
    const uint32_t dst = self->parent_->codec_sample_rate();
    const uint32_t src = self->src_rate_;
    if (src == 0 || src == dst || dst == 0 || dst % src != 0) {
      // Same rate (e.g. media_player at 48 kHz) or unknown -> passthrough.
      self->parent_->write_speaker(buf.data(), got);
    } else {
      // Integer upsample src -> dst (e.g. 16 kHz -> 48 kHz = x3) with linear
      // interpolation and continuity across chunks (resamp_last_).
      const uint32_t up = dst / src;
      const int16_t *s = reinterpret_cast<const int16_t *>(buf.data());
      const size_t n = got / sizeof(int16_t);
      self->resamp_buf_.clear();
      self->resamp_buf_.reserve(n * up);
      for (size_t i = 0; i < n; i++) {
        const int16_t cur = s[i];
        const int16_t prev = (i == 0) ? self->resamp_last_ : s[i - 1];
        for (uint32_t k = 0; k < up; k++) {
          const int32_t v = (int32_t) prev + (int32_t)(cur - prev) * (int32_t) k / (int32_t) up;
          self->resamp_buf_.push_back((int16_t) v);
        }
      }
      if (n > 0)
        self->resamp_last_ = s[n - 1];
      self->parent_->write_speaker(reinterpret_cast<const uint8_t *>(self->resamp_buf_.data()),
                                   self->resamp_buf_.size() * sizeof(int16_t));
    }
  }
  self->parent_->engine_stop();
  self->state_ = speaker::STATE_STOPPED;
  self->task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace fdaudio
}  // namespace esphome
