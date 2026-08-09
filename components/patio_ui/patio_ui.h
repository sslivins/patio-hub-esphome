#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP_IDF
#include <atomic>
#include <string>

// LVGL (v9, pulled in as an IDF component via esp_lvgl_port). Forward-declare the
// handle type so the header stays light; the .cpp pulls in the full LVGL headers.
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_timer_t;
typedef struct _lv_timer_t lv_timer_t;

#include "esphome/components/api/custom_api_device.h"

namespace esphome {
namespace patio_ui {

/*
 * PatioUI — owns the LVGL layer directly (via the M5Stack Core2 BSP:
 * esp_lcd + esp_lvgl_port DMA flush) for a smooth, native-feeling UI, while
 * riding on ESPHome for WiFi / native API / OTA. The heater tile drives the
 * Home Assistant patio-heater timer through the native API (CustomAPIDevice).
 *
 * Threading model (important):
 *   - All LVGL objects are only ever touched from the esp_lvgl_port task:
 *     button event callbacks and the 1 Hz lv_timer both run there.
 *   - Home Assistant state callbacks and Component::loop() run on the ESPHome
 *     main-loop / API task and ONLY touch std::atomic members (never LVGL).
 *   This lock-free split avoids any cross-task LVGL access.
 */
class PatioUI : public Component, public api::CustomAPIDevice {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void dump_config() override;

  // YAML-configurable Home Assistant contract.
  void set_run_script(const std::string &s) { this->run_script_ = s; }
  void set_stop_script(const std::string &s) { this->stop_script_ = s; }
  void set_timer_entity(const std::string &s) { this->timer_entity_ = s; }
  void set_default_minutes(int m) { this->setpoint_minutes_ = m; }
  void set_min_minutes(int m) { this->min_minutes_ = m; }
  void set_max_minutes(int m) { this->max_minutes_ = m; }

  // Button intents (called from the LVGL task; only touch atomics).
  void request_start();
  void request_stop();
  void adjust_setpoint(int delta);

  // --- perimeter screen controls ---
  static constexpr int NUM_SCREENS = 4;   // left, right, rear_left, rear_right

  // YAML codegen: bind a screen slot to a cover entity + display label.
  void add_screen(int slot, const std::string &entity, const std::string &label);

  // Screen intents (called from the LVGL task; touch LVGL + selection only).
  void toggle_screen_sel(int idx);
  void toggle_all_sel();
  void request_cover_action(int action);  // 1=open, 2=close, 3=stop (uses selection)

  // Binds a screen index to its LVGL tap callback.
  struct ScreenTap {
    PatioUI *self{nullptr};
    int idx{0};
  };

 protected:
  // --- display / UI bring-up ---
  void build_ui_();

  // --- live screen capture (uncompressed PNG over HTTP on :8080) ---
  void start_screenshot_server_();
  void *screenshot_httpd_{nullptr};  // httpd_handle_t (opaque; kept void* to keep header light)

  // --- Home Assistant state subscriptions (run on main/API task) ---
  void on_timer_state_(std::string state);
  void on_timer_remaining_(std::string remaining);

  // --- LVGL-task helpers ---
  static void tick_cb_(lv_timer_t *t);
  void tick_();               // 1 Hz, LVGL task
  void refresh_heater_label_();  // LVGL task only
  void build_screens_tile_(lv_obj_t *tile);  // LVGL task only
  void update_screen_visual_();              // LVGL task only

  // --- config ---
  std::string run_script_{"script.patio_heater_run"};
  std::string stop_script_{"script.patio_heater_stop"};
  std::string timer_entity_{"timer.patio_heaters"};
  int min_minutes_{5};
  int max_minutes_{480};

  // --- LVGL widgets (LVGL task only) ---
  lv_obj_t *heater_value_{nullptr};
  lv_timer_t *tick_timer_{nullptr};

  // screen tile widgets (LVGL task only)
  lv_obj_t *screen_btn_[NUM_SCREENS]{};
  lv_obj_t *all_btn_{nullptr};

  // --- screen config / selection ---
  std::string screen_entity_[NUM_SCREENS];
  std::string screen_label_[NUM_SCREENS];
  bool screen_configured_[NUM_SCREENS]{};
  ScreenTap screen_tap_[NUM_SCREENS];
  bool screen_sel_[NUM_SCREENS]{};  // LVGL task only

  // --- cross-task state (atomics) ---
  std::atomic<int> setpoint_minutes_{30};     // desired run length
  std::atomic<int> countdown_secs_{-1};       // remaining secs when active, else -1
  std::atomic<bool> active_{false};           // HA timer is running
  std::atomic<int> pending_start_{-1};        // minutes to start, -1 = none
  std::atomic<bool> pending_stop_{false};
  std::atomic<bool> label_dirty_{true};       // request LVGL-task label refresh

  // screens: UI -> HA pending cover action (Somfy RTS = command-only, no state)
  std::atomic<int> pending_cover_action_{0};     // 0 none,1 open/up,2 close/down,3 stop
  std::atomic<unsigned> pending_cover_mask_{0};  // bitmask of selected screens
};

}  // namespace patio_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
