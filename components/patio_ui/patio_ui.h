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
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/preferences.h"

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

  // Swipeable tile order (a clock/temperature tile leads, then the controls).
  static constexpr int TILE_TIME = 0;
  static constexpr int TILE_HEATER = 1;
  static constexpr int TILE_LIGHTS = 2;
  static constexpr int TILE_SCREENS = 3;
  static constexpr int NUM_TILES = 4;
  // Revert to the clock tile (or the heater tile if a timer is running) after
  // this much untouched time.
  static constexpr uint32_t IDLE_REVERT_MS = 60000;

  // YAML-configurable Home Assistant contract.
  void set_run_script(const std::string &s) { this->run_script_ = s; }
  void set_stop_script(const std::string &s) { this->stop_script_ = s; }
  void set_timer_entity(const std::string &s) { this->timer_entity_ = s; }
  void set_temp_sensor(const std::string &s) { this->temp_sensor_ = s; }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_default_minutes(int m) {
    this->default_minutes_ = m;
    this->setpoint_minutes_ = m;
  }
  void set_min_minutes(int m) { this->min_minutes_ = m; }
  void set_max_minutes(int m) { this->max_minutes_ = m; }

  // Button intents (called from the LVGL task; only touch atomics).
  void request_start();
  void request_stop();
  void request_extend(int add_min);   // +N min: restart run_script with a larger total
  void on_heater_roller_changed();    // scroll picker moved: store minutes
  void on_heater_cancel();            // left button: stop if running, else reset picker
  void on_heater_action();            // right button: Start (idle) / +15 min (running)
  void adjust_setpoint(int delta);

  // --- perimeter screen controls ---
  static constexpr int NUM_SCREENS = 4;   // left, right, rear_left, rear_right
  // YAML codegen: bind a screen slot to a cover entity + display label.
  void add_screen(int slot, const std::string &entity, const std::string &label);

  // Screen intents (called from the LVGL task; touch LVGL + selection only).
  void toggle_screen_sel(int idx);
  void request_cover_action(int action);  // 1=open, 2=close, 3=stop (uses selection)

  // Refreshes the bottom page-position dots (called from the tileview scroll cb).
  void update_page_dots_();  // LVGL task only

  // Binds a screen index to its LVGL tap callback.
  struct ScreenTap {
    PatioUI *self{nullptr};
    int idx{0};
  };

  // --- patio light controls ---
  static constexpr int NUM_LIGHTS = 2;   // main, bbq

  // YAML codegen: bind a light slot to a light entity + display label.
  void add_light(int slot, const std::string &entity, const std::string &label);

  // Light intents (called from the LVGL task; only touch atomics).
  void request_light_brightness(int idx, int pct);  // 0..100 (0 => off)
  void request_light_toggle(int idx);
  void update_light_fill_(int i, int value, bool on);  // LVGL task — size the peach fill layer

  // Binds a light index to its LVGL slider / name-tap callbacks.
  struct LightCtrl {
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
  void on_timer_finishes_at_(std::string finishes_at);  // absolute UTC end time
  void on_light_state_(std::string entity_id, std::string state);       // "on"/"off"
  void on_light_bright_(std::string entity_id, std::string brightness);  // 0..255
  void on_outside_temp_(std::string state);                              // clock-tile temperature
  void on_temp_unit_(std::string unit);                                  // temp sensor's unit (°C/°F)
  int light_index_for_entity_(const std::string &entity_id) const;

  // --- LVGL-task helpers ---
  static void tick_cb_(lv_timer_t *t);
  void tick_();               // 1 Hz, LVGL task
  static void flash_cb_(lv_timer_t *t);
  void flash_tick_();         // fast red/white flash in the final seconds, LVGL task
  void refresh_heater_ui_();  // LVGL task only — dial value/sub + arc
  void build_time_tile_(lv_obj_t *tile);     // LVGL task only
  void refresh_time_tile_();                 // LVGL task only — clock + temperature
  void maybe_auto_revert_();                 // LVGL task only — idle -> clock/heater tile
  void build_screens_tile_(lv_obj_t *tile);  // LVGL task only
  void update_screen_visual_();              // LVGL task only
  void build_lights_tile_(lv_obj_t *tile);   // LVGL task only
  void refresh_lights_ui_();                 // LVGL task only

  // --- config ---
  std::string run_script_{"script.patio_heater_run"};
  std::string stop_script_{"script.patio_heater_stop"};
  std::string timer_entity_{"timer.patio_heaters"};
  std::string temp_sensor_{"sensor.usl_environmental_temperature_3"};
  time::RealTimeClock *time_{nullptr};
  int min_minutes_{5};
  int max_minutes_{480};

  // --- LVGL widgets (LVGL task only) ---
  lv_obj_t *heater_value_{nullptr};       // big MM:SS countdown (running)
  lv_obj_t *heater_roller_{nullptr};      // scroll picker (idle)
  lv_obj_t *heater_btn_left_{nullptr};    // Cancel
  lv_obj_t *heater_btn_right_{nullptr};   // Start / +15 min
  lv_obj_t *heater_btn_right_lbl_{nullptr};
  lv_timer_t *tick_timer_{nullptr};
  lv_timer_t *flash_timer_{nullptr};   // final-seconds red/white flasher
  bool flash_on_{false};               // current flash phase (LVGL task only)
  bool flashing_{false};               // true while the flasher owns the tile

  // tileview + bottom page-position dots (LVGL task only)
  lv_obj_t *tv_{nullptr};
  lv_obj_t *tiles_[NUM_TILES]{};
  lv_obj_t *page_dots_[NUM_TILES]{};

  // clock/temperature tile widgets (LVGL task only)
  lv_obj_t *time_date_{nullptr};   // small header: weekday + date
  lv_obj_t *time_big_{nullptr};    // large 12-hour H:MM
  lv_obj_t *time_ampm_{nullptr};   // AM/PM
  lv_obj_t *temp_label_{nullptr};  // outside temperature (°C)

  // screen tile widgets (LVGL task only)
  lv_obj_t *screen_btn_[NUM_SCREENS]{};
  lv_obj_t *ctrl_up_{nullptr};
  lv_obj_t *ctrl_stop_{nullptr};
  lv_obj_t *ctrl_down_{nullptr};

  // light tile widgets (LVGL task only)
  lv_obj_t *light_slider_[NUM_LIGHTS]{};
  lv_obj_t *light_name_[NUM_LIGHTS]{};
  lv_obj_t *light_fill_[NUM_LIGHTS]{};   // manual peach fill layer (reaches track bottom)

  // --- screen config / selection ---
  std::string screen_entity_[NUM_SCREENS];
  std::string screen_label_[NUM_SCREENS];
  bool screen_configured_[NUM_SCREENS]{};
  ScreenTap screen_tap_[NUM_SCREENS];
  bool screen_sel_[NUM_SCREENS]{};  // LVGL task only

  // --- light config ---
  std::string light_entity_[NUM_LIGHTS];
  std::string light_label_[NUM_LIGHTS];
  bool light_configured_[NUM_LIGHTS]{};
  LightCtrl light_ctrl_[NUM_LIGHTS];

  // --- cross-task state (atomics) ---
  std::atomic<int> setpoint_minutes_{30};     // desired run length
  int default_minutes_{30};                   // configured default; picker resets here when idle
  std::atomic<int> countdown_secs_{-1};       // remaining secs when active, else -1
  // Absolute UTC epoch the active HA timer finishes at (0 = unknown). When set
  // and the device clock is valid, the countdown is derived from this each tick
  // so a mid-run reboot resumes at the true remaining time (HA's `remaining`
  // attribute is frozen while running; only `finishes_at` is accurate).
  std::atomic<long> finishes_at_epoch_{0};
  // Persisted copy of finishes_at_epoch_ (NVS). Restored in setup() so a warm
  // reboot shows the countdown immediately from the retained RTC clock, instead
  // of waiting ~30 s for HA to reconnect and re-push the timer state.
  ESPPreferenceObject finishes_pref_;
  void persist_finishes_at_(long epoch);
  std::atomic<bool> active_{false};           // HA timer is running
  std::atomic<int> pending_start_{-1};        // minutes to start, -1 = none
  std::atomic<int> pending_extend_secs_{-1};  // new total secs for timer.start, -1 = none
  std::atomic<bool> pending_stop_{false};
  std::atomic<bool> label_dirty_{true};       // request LVGL-task label refresh

  // screens: UI -> HA pending cover action (Somfy RTS = command-only, no state)
  std::atomic<int> pending_cover_action_{0};     // 0 none,1 open/up,2 close/down,3 stop
  std::atomic<unsigned> pending_cover_mask_{0};  // bitmask of selected screens

  // lights: bidirectional (lights report state, unlike Somfy).
  //   HA -> UI: last known on/off + brightness, refreshed on the LVGL task.
  //   UI -> HA: a pending brightness set and/or a pending on/off toggle.
  std::atomic<int> light_bright_[NUM_LIGHTS];          // 0..100 last brightness from HA
  std::atomic<bool> light_on_[NUM_LIGHTS];             // on/off from HA
  std::atomic<bool> light_ui_dirty_{false};            // HA changed -> refresh sliders
  std::atomic<int> pending_light_bright_[NUM_LIGHTS];  // -1 none, else 0..100 to send
  std::atomic<bool> pending_light_toggle_[NUM_LIGHTS]; // request on/off toggle

  // clock tile: outside temperature pushed from HA.
  std::atomic<float> outside_temp_raw_{0.0f};  // value in the sensor's native unit
  std::atomic<bool> temp_is_f_{false};    // native unit is °F -> convert to °C for display
  std::atomic<bool> temp_valid_{false};   // false until a numeric value arrives
  std::atomic<bool> temp_dirty_{true};    // HA changed -> refresh the label
};

}  // namespace patio_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
