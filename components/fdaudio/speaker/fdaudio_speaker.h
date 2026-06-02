#pragma once

#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "../fdaudio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>

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
  std::unique_ptr<RingBuffer> ring_;
  TaskHandle_t task_{nullptr};
  volatile bool want_run_{false};
};

}  // namespace fdaudio
}  // namespace esphome
