#include "fdaudio_speaker.h"
#include "esphome/core/log.h"

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
    self->parent_->write_speaker(buf.data(), got);
  }
  self->parent_->engine_stop();
  self->state_ = speaker::STATE_STOPPED;
  self->task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace fdaudio
}  // namespace esphome
