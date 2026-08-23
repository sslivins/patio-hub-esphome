#include "patio_speaker.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <driver/i2c_master.h>
#include <esp_timer.h>

#include "bsp/esp-bsp.h"

#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

namespace esphome::patio_ui {

static const size_t TASK_STACK_SIZE = 4096;
static const ssize_t TASK_PRIORITY = 23;

// Must match the ES7210 microphone platform: esp_codec_dev_open reports a
// "conflict sample_rate" if the paired duplex RX/TX rates differ.
static const uint32_t SAMPLE_RATE = 16000;
static const uint8_t BITS_PER_SAMPLE = 16;
static const uint8_t CHANNELS = 1;

// ~0.5 s of 16 kHz mono 16-bit audio buffered in PSRAM.
static const size_t RING_BUFFER_BYTES = SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * CHANNELS / 2;

// Largest slice pulled from the ring buffer and handed to esp_codec_dev_write.
static const size_t WRITE_CHUNK_BYTES = 1024;

// After audio has flowed at least once, if the ring buffer stays empty for this
// long the drain task auto-finishes (state -> STOPPED). This makes is_running()
// semantically correct when playback drains: voice_assistant gates its
// RESPONSE_FINISHED completion on `has_buffered_data() || is_running()`, and our
// on_end automation waits on `not speaker.is_playing`. Without this, the base
// Speaker stays STATE_RUNNING forever (it only leaves RUNNING on an explicit
// stop()/finish()), wedging both. Must exceed the worst-case inter-chunk gap of
// an in-flight TTS stream so we don't tear down the codec mid-reply.
static const uint32_t IDLE_FINISH_MS = 500;

static const char *const TAG = "patio_ui.speaker";

enum SpeakerEventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),    // hard stop: discard buffered audio, set/cleared by ``loop``
  COMMAND_FINISH = (1 << 1),  // soft stop: drain the buffer then stop, set/cleared by ``loop``

  TASK_STARTING = (1 << 10),  // set by spk task, cleared by ``loop``
  TASK_RUNNING = (1 << 11),   // set by spk task, cleared by ``loop``
  TASK_STOPPED = (1 << 13),   // set by spk task, cleared by ``loop``

  ALL_BITS = 0x00FFFFFF,
};

void PatioSpeaker::setup() {
  this->event_group_ = xEventGroupCreate();
  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Creating event group failed");
    this->mark_failed();
    return;
  }

  // Create the AW88298 codec handle once. The BSP init is idempotent and also
  // brings up the shared I2C bus + duplex I2S bus + AW9523 speaker enable.
  this->codec_ = bsp_audio_codec_speaker_init();
  if (this->codec_ == nullptr) {
    ESP_LOGE(TAG, "AW88298 speaker codec init failed");
    this->mark_failed();
    return;
  }

  this->set_audio_stream_info(audio::AudioStreamInfo(BITS_PER_SAMPLE, CHANNELS, SAMPLE_RATE));
}

void PatioSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Patio Speaker (AW88298):\n"
                "  Sample rate: %" PRIu32 " Hz",
                SAMPLE_RATE);
}

void PatioSpeaker::apply_loudness_fix_() {
  // The BSP configures the AW88298 with hw_gain.pa_gain=15, so the driver's
  // set_vol() subtracts 15 dB -> esp_codec_dev_set_out_vol(spk,100) only reaches
  // ~-15 dB digital. Force the volume register (0x0C) to true 0 dB directly over
  // the BSP's shared I2C bus (AW88298 @ 0x36). This is a DIGITAL-only write (high
  // byte = attenuation; 0x00 = 0 dB, low byte 0x64 as the driver uses) -- no
  // analog/boost change, so no hardware risk. The boost converter (REG61) is
  // already enabled at 0x0673 by the driver.
  // TODO(upstream): esp-bsp buries pa_gain=15 with no way to reach 0 dB via the
  // public esp_codec_dev API -- expose pa_gain or drop the offset.
  esp_codec_dev_set_out_vol(this->codec_, 100);  // is_open==true -> writes -15 dB
  i2c_master_bus_handle_t abus = bsp_i2c_get_handle();
  if (abus == nullptr) {
    ESP_LOGW(TAG, "Loudness fix: no I2C bus handle");
    return;
  }
  i2c_device_config_t aw_cfg = {};
  aw_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  aw_cfg.device_address = 0x36;  // AW88298
  aw_cfg.scl_speed_hz = 100000;
  i2c_master_dev_handle_t aw = nullptr;
  if (i2c_master_bus_add_device(abus, &aw_cfg, &aw) == ESP_OK) {
    const uint8_t vol_0db[] = {0x0C, 0x00, 0x64};  // REG0C hi=0x00 -> 0 dB
    if (i2c_master_transmit(aw, vol_0db, sizeof(vol_0db), 1000) != ESP_OK)
      ESP_LOGW(TAG, "Loudness fix: AW88298 vol write failed");
    i2c_master_bus_rm_device(aw);
  }
}

size_t PatioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed())
    return 0;

  // Lazily start the driver on first audio (voice_assistant does not always call
  // start() before feeding the media player).
  if (this->state_ == speaker::STATE_STOPPED) {
    this->start();
  }

  if (this->audio_ring_buffer_ == nullptr)
    return 0;

  return this->audio_ring_buffer_->write_without_replacement(data, length, ticks_to_wait);
}

void PatioSpeaker::start() {
  if (this->is_failed())
    return;
  if (this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STARTING)
    return;
  this->state_ = speaker::STATE_STARTING;
}

bool PatioSpeaker::start_driver_() {
  if (this->audio_ring_buffer_ == nullptr) {
    this->audio_ring_buffer_ = ring_buffer::RingBuffer::create(RING_BUFFER_BYTES);
    if (this->audio_ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate ring buffer");
      return false;
    }
  } else {
    this->audio_ring_buffer_->reset();
  }

  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = BITS_PER_SAMPLE;
  fs.channel = CHANNELS;
  fs.channel_mask = 0;
  fs.sample_rate = SAMPLE_RATE;
  fs.mclk_multiple = 0;

  int ret = esp_codec_dev_open(this->codec_, &fs);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "AW88298 open failed: %d", ret);
    return false;
  }

  this->apply_loudness_fix_();
  return true;
}

void PatioSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED || this->is_failed())
    return;
  xEventGroupSetBits(this->event_group_, SpeakerEventGroupBits::COMMAND_STOP);
}

void PatioSpeaker::finish() {
  if (this->state_ == speaker::STATE_STOPPED || this->is_failed())
    return;
  xEventGroupSetBits(this->event_group_, SpeakerEventGroupBits::COMMAND_FINISH);
}

void PatioSpeaker::stop_driver_() {
  if (this->codec_ != nullptr) {
    esp_codec_dev_close(this->codec_);
  }
}

bool PatioSpeaker::has_buffered_data() const {
  if (this->audio_ring_buffer_ == nullptr)
    return false;
  return this->audio_ring_buffer_->available() > 0;
}

void PatioSpeaker::spk_task(void *params) {
  PatioSpeaker *this_speaker = (PatioSpeaker *) params;
  xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_STARTING);

  {  // Ensures the buffer is freed when the task stops
    std::vector<uint8_t> buffer;
    buffer.resize(WRITE_CHUNK_BYTES);

    xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_RUNNING);

    bool played_any = false;
    uint32_t last_data_ms = millis();

    while (true) {
      uint32_t bits = xEventGroupGetBits(this_speaker->event_group_);
      if (bits & SpeakerEventGroupBits::COMMAND_STOP) {
        break;
      }

      size_t available = this_speaker->audio_ring_buffer_->available();
      if ((bits & SpeakerEventGroupBits::COMMAND_FINISH) && (available == 0)) {
        break;  // buffer drained -> honor the soft stop
      }

      if (available == 0) {
        // Auto-finish once playback has drained so is_running() reflects reality
        // (see IDLE_FINISH_MS). Guard on played_any so the brief window between
        // lazy start() and voice_assistant's first write does not tear us down.
        if (played_any && (millis() - last_data_ms) >= IDLE_FINISH_MS) {
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      size_t to_read = available < WRITE_CHUNK_BYTES ? available : WRITE_CHUNK_BYTES;
      size_t bytes_read = this_speaker->audio_ring_buffer_->read(buffer.data(), to_read, pdMS_TO_TICKS(20));
      if (bytes_read == 0)
        continue;

      int ret = esp_codec_dev_write(this_speaker->codec_, buffer.data(), bytes_read);
      if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Write error: %d", ret);
        continue;
      }

      played_any = true;
      last_data_ms = millis();

      uint32_t frames = this_speaker->audio_stream_info_.bytes_to_frames(bytes_read);
      this_speaker->audio_output_callback_.call(frames, esp_timer_get_time());
    }
  }

  xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_STOPPED);
  while (true) {
    // Continuously delay until the loop method deletes the task
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void PatioSpeaker::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & SpeakerEventGroupBits::TASK_STARTING) {
    ESP_LOGV(TAG, "Task started");
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::TASK_STARTING);
  }

  if (event_group_bits & SpeakerEventGroupBits::TASK_RUNNING) {
    ESP_LOGV(TAG, "Task is running and draining audio");
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::TASK_RUNNING);
    this->state_ = speaker::STATE_RUNNING;
  }

  if (event_group_bits & SpeakerEventGroupBits::TASK_STOPPED) {
    ESP_LOGV(TAG, "Task finished, closing codec");
    vTaskDeleteWithCaps(this->task_handle_);
    this->task_handle_ = nullptr;
    this->stop_driver_();
    if (this->audio_ring_buffer_ != nullptr)
      this->audio_ring_buffer_->reset();
    xEventGroupClearBits(this->event_group_, ALL_BITS);
    this->status_clear_error();
    this->state_ = speaker::STATE_STOPPED;
  }

  switch (this->state_) {
    case speaker::STATE_STARTING:
      if (this->status_has_error()) {
        break;
      }

      if (!this->start_driver_()) {
        ESP_LOGE(TAG, "Driver failed to start; retrying in 1 second");
        this->status_momentary_error("driver_fail", 1000);
        this->stop_driver_();
        break;
      }

      if (this->task_handle_ == nullptr) {
        // Stack in PSRAM; pinned to core 1 to keep audio off core 0 (main loop
        // + WiFi live there). Stack size is in BYTES on ESP-IDF.
        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
            PatioSpeaker::spk_task, "spk_task", TASK_STACK_SIZE, (void *) this, TASK_PRIORITY,
            &this->task_handle_, 1, MALLOC_CAP_SPIRAM);

        if (ret != pdPASS || this->task_handle_ == nullptr) {
          ESP_LOGE(TAG, "Task failed to start, retrying in 1 second");
          this->task_handle_ = nullptr;
          this->status_momentary_error("task_fail", 1000);
          this->stop_driver_();
        }
      }
      break;
    case speaker::STATE_RUNNING:
      break;
    case speaker::STATE_STOPPING:
      break;
    case speaker::STATE_STOPPED:
      break;
  }
}

}  // namespace esphome::patio_ui

#endif  // USE_ESP32
