#pragma once

// Only compiled when the `speaker:` platform is actually declared in the YAML
// (USE_SPEAKER). A minimal build that omits the speaker platform (e.g. the
// call-button device) otherwise fails on the missing speaker base header.
#if defined(USE_ESP32) && defined(USE_SPEAKER)

#include "esphome/components/speaker/speaker.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <memory>

#include "esp_codec_dev.h"

namespace esphome::patio_ui {

// ESPHome `speaker` platform for the M5Stack CoreS3's AW88298 mono amp/speaker.
// Writes through the esp-bsp / esp_codec_dev stack (bsp_audio_codec_speaker_init)
// so it shares the BSP-owned duplex I2S bus + I2C control bus + AW9523 enable
// with the ES7210 microphone platform. Both surfaces MUST run at 16 kHz.
//
// Design: play() enqueues PCM into a PSRAM ring buffer; a drain task pulls from
// it and blocks in esp_codec_dev_write(). This keeps the caller (voice_assistant
// media pipeline) non-blocking while the I2S DMA paces playback. After the codec
// opens we force the AW88298 to true 0 dB (the BSP's pa_gain=15 otherwise caps
// output ~15 dB low) via a raw I2C write, matching the confirmed loudness fix.
class PatioSpeaker final : public speaker::Speaker, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  float get_setup_priority() const override { return setup_priority::LATE; }

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;

  bool has_buffered_data() const override;

 protected:
  bool start_driver_();
  void stop_driver_();
  void apply_loudness_fix_();

  static void spk_task(void *params);

  std::unique_ptr<ring_buffer::RingBuffer> audio_ring_buffer_;
  EventGroupHandle_t event_group_{nullptr};
  TaskHandle_t task_handle_{nullptr};

  esp_codec_dev_handle_t codec_{nullptr};
};

}  // namespace esphome::patio_ui

#endif  // USE_ESP32
