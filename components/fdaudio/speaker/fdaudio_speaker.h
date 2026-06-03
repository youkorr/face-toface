#pragma once

#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "../fdaudio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace esphome {
namespace fdaudio {

class FdAudioSpeaker : public Component, public speaker::Speaker {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  void start() override;
  void stop() override;
  size_t play(const uint8_t *data, size_t length) override;
  bool has_buffered_data() const override;

  void set_parent(FdAudio *parent) { parent_ = parent; }

 protected:
  static void write_task_(void *param);

  FdAudio *parent_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  TaskHandle_t task_{nullptr};
  volatile bool want_run_{false};
  // Source sample rate of the data being played (captured in play() from the
  // stream info). The engine clocks the codec at 48 kHz, so 16 kHz callers
  // (face2face, ringtone) are upsampled in write_task_; media_player already
  // sends 48 kHz -> passthrough.
  volatile uint32_t src_rate_{0};
  int16_t resamp_last_{0};
  std::vector<int16_t> resamp_buf_;
};

}  // namespace fdaudio
}  // namespace esphome
