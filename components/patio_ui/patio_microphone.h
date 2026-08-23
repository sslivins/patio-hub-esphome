#pragma once

#ifdef USE_ESP32

#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_codec_dev.h"

namespace esphome::patio_ui {

// ESPHome `microphone` platform for the M5Stack CoreS3's ES7210 dual-mic ADC.
// Unlike the stock i2s_audio microphone (which owns a raw I2S RX channel), this
// reads through the esp-bsp / esp_codec_dev stack (bsp_audio_codec_microphone_init)
// so it shares the BSP-owned duplex I2S bus + I2C control bus + AW9523 enable
// with the AW88298 speaker platform. Both surfaces MUST run at the same sample
// rate (16 kHz): esp_codec_dev errors if the paired RX/TX rates conflict.
//
// The lifecycle mirrors the stock i2s_audio microphone: a counting semaphore
// tracks active listeners (so micro_wake_word AND voice_assistant can each
// start/stop independently), an event group drives a capture task, and captured
// PCM is pushed to consumers via data_callbacks_.
class PatioMicrophone final : public microphone::Microphone, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  void start() override;
  void stop() override;

  // After display/PSRAM the ES7210 handle must be created on the BSP bus; keep
  // this after the BSP is up. DATA runs late enough that the bus is ready.
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_gain_db(float gain_db) { this->gain_db_ = gain_db; }

 protected:
  bool start_driver_();
  void stop_driver_();
  size_t read_(uint8_t *buf, size_t len);
  void configure_stream_settings_();

  static void mic_task(void *params);

  SemaphoreHandle_t active_listeners_semaphore_{nullptr};
  EventGroupHandle_t event_group_{nullptr};
  TaskHandle_t task_handle_{nullptr};

  esp_codec_dev_handle_t codec_{nullptr};
  float gain_db_{30.0f};
};

}  // namespace esphome::patio_ui

#endif  // USE_ESP32
