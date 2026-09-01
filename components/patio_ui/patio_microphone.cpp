#include "patio_microphone.h"

#if defined(USE_ESP32) && defined(USE_MICROPHONE)

#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "bsp/esp-bsp.h"

#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#ifdef PATIO_AEC
#include <cstring>
#include "esp_aec.h"
#include "patio_aec_ref.h"
#endif

namespace esphome::patio_ui {

// Multiple consumers (micro_wake_word + voice_assistant) can each hold a
// listener slot; the mic runs while any slot is taken.
static const UBaseType_t MAX_LISTENERS = 16;

static const uint32_t READ_DURATION_MS = 16;

static const size_t TASK_STACK_SIZE = 4096;
static const ssize_t TASK_PRIORITY = 23;

// esp_codec_dev + the BSP duplex I2S bus are locked to a single sample rate; it
// MUST match the AW88298 speaker platform or esp_codec_dev_open reports a
// "conflict sample_rate". 16 kHz is what micro_wake_word / voice_assistant want.
static const uint32_t SAMPLE_RATE = 16000;
static const uint8_t BITS_PER_SAMPLE = 16;
static const uint8_t CHANNELS = 1;

static const char *const TAG = "patio_ui.microphone";

enum MicrophoneEventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),  // stops the microphone task, set and cleared by ``loop``

  TASK_STARTING = (1 << 10),  // set by mic task, cleared by ``loop``
  TASK_RUNNING = (1 << 11),   // set by mic task, cleared by ``loop``
  TASK_STOPPED = (1 << 13),   // set by mic task, cleared by ``loop``

  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

void PatioMicrophone::setup() {
  this->active_listeners_semaphore_ = xSemaphoreCreateCounting(MAX_LISTENERS, MAX_LISTENERS);
  if (this->active_listeners_semaphore_ == nullptr) {
    ESP_LOGE(TAG, "Creating semaphore failed");
    this->mark_failed();
    return;
  }

  this->event_group_ = xEventGroupCreate();
  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Creating event group failed");
    this->mark_failed();
    return;
  }

  // Create the ES7210 codec handle once. The BSP init is idempotent and also
  // brings up the shared I2C bus + duplex I2S bus + AW9523 mic/speaker enable,
  // so this is safe regardless of whether the speaker platform ran first.
  this->codec_ = bsp_audio_codec_microphone_init();
  if (this->codec_ == nullptr) {
    ESP_LOGE(TAG, "ES7210 microphone codec init failed");
    this->mark_failed();
    return;
  }

  this->configure_stream_settings_();

#ifdef PATIO_AEC
  this->init_aec_();
#endif
}

#ifdef PATIO_AEC
void PatioMicrophone::init_aec_() {
  aec_config_t cfg = {};
  cfg.mic_num = 1;
  cfg.ref_num = 1;
  cfg.out_num = 1;
  cfg.filter_length = 4;  // Espressif-recommended for ESP32-S3
  cfg.sample_rate = SAMPLE_RATE;
  // Push the AEC's own working buffers to PSRAM (internal RAM is scarce here);
  // only our small per-frame scratch buffers stay internal.
  cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  cfg.mode = AEC_MODE_FD_LOW_COST;   // full-duplex: linear filter + nonlinear post-filter
  cfg.nlp_level = AEC_NLP_LEVEL_AGGR;

  aec_handle_t *handle = aec_create_from_config(&cfg);
  if (handle == nullptr) {
    ESP_LOGE(TAG, "AEC create failed; running half-duplex (no barge-in)");
    return;
  }

  this->aec_frame_samples_ = (size_t) aec_get_chunksize(handle);
  const size_t bytes = this->aec_frame_samples_ * sizeof(int16_t);
  // esp_aec requires 16-bit signed buffers allocated with an aligned allocator.
  this->aec_mic_ = (int16_t *) heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  this->aec_ref_ = (int16_t *) heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  this->aec_out_ = (int16_t *) heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (this->aec_mic_ == nullptr || this->aec_ref_ == nullptr || this->aec_out_ == nullptr) {
    ESP_LOGE(TAG, "AEC scratch alloc failed; running half-duplex");
    aec_destroy(handle);
    this->aec_frame_samples_ = 0;
    return;
  }

  this->aec_handle_ = (void *) handle;
  ESP_LOGCONFIG(TAG, "AEC ready: FD_LOW_COST, frame=%u samples", (unsigned) this->aec_frame_samples_);
}
#endif

void PatioMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Patio Microphone (ES7210):\n"
                "  Sample rate: %" PRIu32 " Hz\n"
                "  Input gain: %.1f dB",
                SAMPLE_RATE, this->gain_db_);
}

void PatioMicrophone::configure_stream_settings_() {
  this->audio_stream_info_ = audio::AudioStreamInfo(BITS_PER_SAMPLE, CHANNELS, SAMPLE_RATE);
}

void PatioMicrophone::start() {
  if (this->is_failed())
    return;

  xSemaphoreTake(this->active_listeners_semaphore_, 0);
}

bool PatioMicrophone::start_driver_() {
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = BITS_PER_SAMPLE;
  fs.channel = CHANNELS;
  fs.channel_mask = 0;
  fs.sample_rate = SAMPLE_RATE;
  fs.mclk_multiple = 0;

  int ret = esp_codec_dev_open(this->codec_, &fs);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "ES7210 open failed: %d", ret);
    return false;
  }

  // Input gain only takes effect once the device is open (mirrors the AW88298
  // out-vol quirk), so apply it after opening.
  esp_codec_dev_set_in_gain(this->codec_, this->gain_db_);

  this->configure_stream_settings_();
  return true;
}

void PatioMicrophone::stop() {
  if (this->state_ == microphone::STATE_STOPPED || this->is_failed())
    return;

  xSemaphoreGive(this->active_listeners_semaphore_);
}

void PatioMicrophone::stop_driver_() {
  // The handle is created once in setup() and reused; only close (do not delete)
  // so the next start() can reopen it.
  if (this->codec_ != nullptr) {
    esp_codec_dev_close(this->codec_);
  }
}

size_t PatioMicrophone::read_(uint8_t *buf, size_t len) {
  // esp_codec_dev_read blocks until the full slice is available (~READ_DURATION_MS
  // at 16 kHz), then returns ESP_CODEC_DEV_OK.
  int ret = esp_codec_dev_read(this->codec_, buf, len);
  if (ret != ESP_CODEC_DEV_OK) {
    if (!this->status_has_warning()) {
      ESP_LOGW(TAG, "Read error: %d", ret);
    }
    this->status_set_warning();
    return 0;
  }
  this->status_clear_warning();
  return len;
}

void PatioMicrophone::mic_task(void *params) {
  PatioMicrophone *this_microphone = (PatioMicrophone *) params;
  xEventGroupSetBits(this_microphone->event_group_, MicrophoneEventGroupBits::TASK_STARTING);

  {  // Ensures the samples vector is freed when the task stops
    size_t bytes_to_read = this_microphone->audio_stream_info_.ms_to_bytes(READ_DURATION_MS);
#ifdef PATIO_AEC
    // When AEC is active, read exactly one AEC frame per iteration so each read
    // maps 1:1 to an aec_process() call and stays sample-locked with the ref.
    const bool use_aec = (this_microphone->aec_handle_ != nullptr) && (this_microphone->aec_frame_samples_ > 0);
    if (use_aec)
      bytes_to_read = this_microphone->aec_frame_samples_ * sizeof(int16_t);
#endif
    std::vector<uint8_t> samples;
    samples.reserve(bytes_to_read);

    xEventGroupSetBits(this_microphone->event_group_, MicrophoneEventGroupBits::TASK_RUNNING);

    while (!(xEventGroupGetBits(this_microphone->event_group_) & MicrophoneEventGroupBits::COMMAND_STOP)) {
      if (this_microphone->data_callbacks_.size() > 0) {
        samples.resize(bytes_to_read);
        size_t bytes_read = this_microphone->read_(samples.data(), bytes_to_read);
#ifdef PATIO_AEC
        // Cancel the speaker's echo out of the captured frame using the far-end
        // reference the speaker task pushed. Requires a full frame; short reads
        // fall through as raw audio.
        if (use_aec && bytes_read == bytes_to_read) {
          const size_t n = this_microphone->aec_frame_samples_;
          std::memcpy(this_microphone->aec_mic_, samples.data(), bytes_read);
          aec_ref_pull(this_microphone->aec_ref_, n);
          aec_process((aec_handle_t *) this_microphone->aec_handle_, this_microphone->aec_mic_,
                      this_microphone->aec_ref_, this_microphone->aec_out_);
          std::memcpy(samples.data(), this_microphone->aec_out_, bytes_read);
        }
#endif
        samples.resize(bytes_read);
        this_microphone->data_callbacks_.call(samples);
      } else {
        vTaskDelay(pdMS_TO_TICKS(READ_DURATION_MS));
      }
    }
  }

  xEventGroupSetBits(this_microphone->event_group_, MicrophoneEventGroupBits::TASK_STOPPED);
  while (true) {
    // Continuously delay until the loop method deletes the task
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void PatioMicrophone::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & MicrophoneEventGroupBits::TASK_STARTING) {
    ESP_LOGV(TAG, "Task started");
    xEventGroupClearBits(this->event_group_, MicrophoneEventGroupBits::TASK_STARTING);
  }

  if (event_group_bits & MicrophoneEventGroupBits::TASK_RUNNING) {
    ESP_LOGV(TAG, "Task is running and reading data");
    xEventGroupClearBits(this->event_group_, MicrophoneEventGroupBits::TASK_RUNNING);
    this->state_ = microphone::STATE_RUNNING;
  }

  if ((event_group_bits & MicrophoneEventGroupBits::TASK_STOPPED)) {
    ESP_LOGV(TAG, "Task finished, closing codec");
    vTaskDeleteWithCaps(this->task_handle_);
    this->task_handle_ = nullptr;
    this->stop_driver_();
    xEventGroupClearBits(this->event_group_, ALL_BITS);
    this->status_clear_error();
    this->state_ = microphone::STATE_STOPPED;
  }

  // Start the microphone if any listeners are active
  if ((uxSemaphoreGetCount(this->active_listeners_semaphore_) < MAX_LISTENERS) &&
      (this->state_ == microphone::STATE_STOPPED)) {
    this->state_ = microphone::STATE_STARTING;
  }

  // Stop the microphone if all listeners returned
  if ((uxSemaphoreGetCount(this->active_listeners_semaphore_) == MAX_LISTENERS) &&
      (this->state_ == microphone::STATE_RUNNING)) {
    this->state_ = microphone::STATE_STOPPING;
  }

  switch (this->state_) {
    case microphone::STATE_STARTING:
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
        // Stack in PSRAM (internal RAM is too fragmented by LVGL + display
        // buffers + the wake-word model). Pinned to core 1: the ESPHome main
        // loop + WiFi live on core 0, so keeping audio off core 0 stops the
        // high-priority read task from starving the API keepalive. Stack size
        // is in BYTES on ESP-IDF.
        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
            PatioMicrophone::mic_task, "mic_task", TASK_STACK_SIZE, (void *) this, TASK_PRIORITY,
            &this->task_handle_, 1, MALLOC_CAP_SPIRAM);

        if (ret != pdPASS || this->task_handle_ == nullptr) {
          ESP_LOGE(TAG, "Task failed to start, retrying in 1 second");
          this->task_handle_ = nullptr;
          this->status_momentary_error("task_fail", 1000);
          this->stop_driver_();
        }
      }
      break;
    case microphone::STATE_RUNNING:
      break;
    case microphone::STATE_STOPPING:
      xEventGroupSetBits(this->event_group_, MicrophoneEventGroupBits::COMMAND_STOP);
      break;
    case microphone::STATE_STOPPED:
      break;
  }
}

}  // namespace esphome::patio_ui

#endif  // USE_ESP32
