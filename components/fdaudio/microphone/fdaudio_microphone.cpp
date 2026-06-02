#include "fdaudio_microphone.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fdaudio {

static const char *const TAG = "fdaudio.mic";

void FdAudioMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, 16000);
}

void FdAudioMicrophone::dump_config() { ESP_LOGCONFIG(TAG, "fdaudio microphone"); }

void FdAudioMicrophone::start() {
  if (this->state_ == microphone::STATE_RUNNING || this->want_run_)
    return;
  if (parent_ == nullptr || !parent_->engine_start()) {
    ESP_LOGE(TAG, "engine start failed");
    return;
  }
  this->state_ = microphone::STATE_STARTING;
  this->want_run_ = true;
  xTaskCreatePinnedToCore(read_task_, "fdaudio_mic", 4096, this, 5, &this->task_, 1);
}

void FdAudioMicrophone::stop() {
  if (!this->want_run_)
    return;
  this->want_run_ = false;
  this->state_ = microphone::STATE_STOPPING;
  // task exits on its own; engine_stop happens there.
}

void FdAudioMicrophone::read_task_(void *param) {
  auto *self = static_cast<FdAudioMicrophone *>(param);
  self->state_ = microphone::STATE_RUNNING;
  // 32 ms @ 16 kHz mono 16-bit = 1024 bytes.
  std::vector<uint8_t> buf(1024);
  while (self->want_run_) {
    size_t got = self->parent_->read_mic(buf.data(), buf.size());
    if (got == 0) {
      vTaskDelay(1);
      continue;
    }
    std::vector<uint8_t> chunk(buf.begin(), buf.begin() + got);
    self->data_callbacks_.call(chunk);
  }
  self->parent_->engine_stop();
  self->state_ = microphone::STATE_STOPPED;
  self->task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace fdaudio
}  // namespace esphome
