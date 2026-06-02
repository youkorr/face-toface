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
  void loop() override;
  void start() override;
  void stop() override;

  void set_parent(FdAudio *parent) { parent_ = parent; }

 protected:
  static void read_task_(void *param);

  FdAudio *parent_{nullptr};
  TaskHandle_t task_{nullptr};
  volatile bool want_run_{false};
  // Reference count of active listeners (micro_wake_word, voice_assistant,
  // face2face...). The read task runs while >0. start()/stop() only adjust this;
  // loop() reconciles the task lifecycle so a mww.stop()+VA.start() handoff
  // (which nets to 1 listener) never actually stops the mic.
  int listeners_{0};
};

}  // namespace fdaudio
}  // namespace esphome
