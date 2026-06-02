#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/audio/audio.h"
#include "../fdaudio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace fdaudio {

class FdAudioMicrophone : public Component, public microphone::Microphone {
 public:
  void setup() override;
  void dump_config() override;
  void start() override;
  void stop() override;

  void set_parent(FdAudio *parent) { parent_ = parent; }

 protected:
  static void read_task_(void *param);

  FdAudio *parent_{nullptr};
  TaskHandle_t task_{nullptr};
  volatile bool want_run_{false};
};

}  // namespace fdaudio
}  // namespace esphome
