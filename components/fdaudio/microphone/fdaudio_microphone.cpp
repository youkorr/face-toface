#include "fdaudio_microphone.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fdaudio {

static const char *const TAG = "fdaudio.mic";

void FdAudioMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, 16000);
}

void FdAudioMicrophone::dump_config() { ESP_LOGCONFIG(TAG, "fdaudio microphone"); }

// start()/stop() are called once per consumer (micro_wake_word, voice_assistant,
// face2face). They only adjust the listener count; the actual task is started or
// stopped in loop() so back-to-back stop()+start() during a handoff don't drop
// the mic.
void FdAudioMicrophone::start() {
  this->listeners_++;
}

void FdAudioMicrophone::stop() {
  if (this->listeners_ > 0)
    this->listeners_--;
}

void FdAudioMicrophone::loop() {
  const bool want = this->listeners_ > 0;
  if (want && this->state_ == microphone::STATE_STOPPED && this->task_ == nullptr) {
    if (parent_ == nullptr || !parent_->engine_start()) {
      ESP_LOGE(TAG, "engine start failed");
      return;
    }
    this->want_run_ = true;
    this->state_ = microphone::STATE_STARTING;
    // 8 KB stack: the esp-sr aec_process() DSP overflows a 4 KB stack (crash on
    // some boards when enable_aec is on).
    xTaskCreatePinnedToCore(read_task_, "fdaudio_mic", 8192, this, parent_->task_priority(),
                            &this->task_, parent_->task_core());
  } else if (!want && this->state_ == microphone::STATE_RUNNING) {
    this->want_run_ = false;  // task exits on its own; engine_stop happens there
    this->state_ = microphone::STATE_STOPPING;
  }
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
