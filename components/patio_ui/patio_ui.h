#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP_IDF
#include <atomic>
#include <string>

#include "freertos/FreeRTOS.h"  // portMUX_TYPE for the cross-task now-playing title
#include "freertos/task.h"

// LVGL (v9, pulled in as an IDF component via esp_lvgl_port). Forward-declare the
// handle type so the header stays light; the .cpp pulls in the full LVGL headers.
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_timer_t;
typedef struct _lv_timer_t lv_timer_t;
struct _lv_anim_t;
typedef struct _lv_anim_t lv_anim_t;

#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/automation.h"
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

  // Tile kinds. The active tile set + order is configured in YAML via `tiles:`
  // (default = the patio set); only the requested tiles are built. These values
  // must match TILE_KINDS in __init__.py.
  enum TileKind : uint8_t {
    TK_TIME = 0,
    TK_HEATER = 1,
    TK_CLIMATE = 2,
    TK_LIGHTS = 3,
    TK_SCREENS = 4,
    TK_MEDIA = 5,
  };
  static constexpr int MAX_TILES = 6;
  // YAML codegen: append a tile kind to the ordered tile set.
  void add_tile(int kind);
  // Revert to the clock tile (or the heater tile if a timer is running) after
  // this much untouched time.
  static constexpr uint32_t IDLE_REVERT_MS = 60000;

  // Turn the backlight off entirely after this much untouched time to avoid
  // burn-in from the static clock. The next touch is swallowed (just wakes).
  static constexpr uint32_t SCREEN_SLEEP_MS = 180000;  // 3 min of inactivity -> eyelids close

  // On battery ONLY: once the screen has been asleep (backlight cut) for this
  // much additional idle time, hand off to the ESP deep_sleep component to save
  // the battery. A screen TAP (FT6336 INT on the RTC-capable GPIO39) reboots and
  // wakes it back into the UI. Never triggers on USB power (the device just
  // stays screen-asleep). Total idle before deep sleep = SCREEN_SLEEP_MS + this.
  static constexpr uint32_t DEEP_SLEEP_AFTER_SCREEN_MS = 120000;  // +2 min after screen sleep

  // The deliberate eyes-opening sweep on wake uses this duration; the
  // "fighting sleep" nod-off close durations are baked into the keyframe table
  // in patio_ui.cpp.
  static constexpr uint32_t SCREEN_OPEN_MS = 520;  // deliberate eyes-open on wake

  // Fired when the device decides to enter deep sleep (on battery + screen
  // idle). The board entrypoint wires this to the ESPHome `deep_sleep`
  // component (GPIO39 tap-wake) via `on_deep_sleep_request`.
  Trigger<> *get_deep_sleep_trigger() { return &this->deep_sleep_trigger_; }

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
  // Tile title (default "Screens") + layout: 0 = perimeter map (patio deck),
  // 1 = horizontal row (side slots single-width, rear/center slots double-width).
  void set_screen_title(const std::string &s) { this->screen_title_ = s; }
  void set_screen_layout(int mode) { this->screen_layout_ = mode; }

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

  // --- deck media player (single Sonos amp) ---
  // YAML codegen: bind the media tile to a media_player entity + display label.
  void set_media_entity(const std::string &s) {
    this->media_entity_ = s;
    this->media_configured_ = true;
  }
  void set_media_label(const std::string &s) { this->media_label_ = s; }

  // --- climate (thermostat) ---
  // YAML codegen: bind the climate tile to a HA climate entity + display label
  // and the setpoint range/step (defaults suit a single-zone mini-split).
  void set_climate_entity(const std::string &s) {
    this->climate_entity_ = s;
    this->climate_configured_ = true;
  }
  void set_climate_label(const std::string &s) { this->climate_label_ = s; }
  void set_climate_min(float v) { this->climate_min_ = v; }
  void set_climate_max(float v) { this->climate_max_ = v; }
  void set_climate_step(float v) { this->climate_step_ = v; }
  // When true, the tile displays/steps in °C while HA speaks °F: incoming
  // temps are converted F->C, and setpoints are converted C->F before sending.
  void set_climate_celsius(bool v) { this->climate_celsius_ = v; }

  // Runtime-adjustable idle timeouts, settable from HA (template number
  // entities in the board yaml). Both are whole seconds of untouched time:
  //   screen  = idle before the screen (backlight) turns off
  //   sleep   = total idle before the device deep-sleeps (on battery only)
  // Defaults preserve the compile-time patio behaviour when never called.
  void set_screen_timeout_s(float s);
  void set_sleep_timeout_s(float s);

  // Climate intents (called from the LVGL task; only touch atomics).
  void request_climate_setpoint(int delta_steps);  // +/- N steps, clamped + optimistic
  void request_climate_mode_index(int idx);         // set mode from the mode dropdown (0..3)
  void request_climate_fan_index(int idx);          // set fan from the fan dropdown (0..3)

  // Media intents (called from the LVGL task; only touch atomics).
  void request_media_cmd(int cmd);        // 1=play/pause, 2=next, 3=prev
  void request_media_volume(int pct);     // 0..100
  void update_media_vol_label_(int pct);  // LVGL task — live "NN%" while dragging
  void go_home_tile();                    // LVGL task — swipe up -> clock tile
  void wake_screen();                     // LVGL task — backlight on, swallow the waking tap

 protected:
  // --- display / UI bring-up ---
  void build_ui_();

#ifdef PATIO_AUDIO
  // CoreS3 only: one-shot hardware proof that plays a tone through the AW88298
  // speaker and logs captured RMS/peak from the ES7210 mics, both via the BSP
  // codec init (which owns the shared I2C bus + AW9523 enable + amp gain).
  void audio_selftest_();
#endif

  // --- live screen capture (uncompressed PNG over HTTP on :8080) ---
  void start_screenshot_server_();
  void *screenshot_httpd_{nullptr};  // httpd_handle_t (opaque; kept void* to keep header light)

  // --- Home Assistant state subscriptions (run on main/API task) ---
  void on_timer_state_(std::string state);
  void on_timer_remaining_(std::string remaining);
  void on_timer_finishes_at_(std::string finishes_at);  // absolute UTC end time
  void on_light_state_(std::string entity_id, std::string state);       // "on"/"off"
  void on_light_bright_(std::string entity_id, std::string brightness);  // 0..255
  void on_screen_position_(std::string entity_id, std::string position); // cover current_position 0..100
  void on_outside_temp_(std::string state);                              // clock-tile temperature
  void on_temp_unit_(std::string unit);                                  // temp sensor's unit (°C/°F)
  void on_media_state_(std::string state);                               // playing/paused/idle
  void on_media_volume_(std::string volume);                             // 0.0..1.0
  void on_media_title_(std::string title);                               // now-playing text
  // Climate (thermostat) state subscriptions.
  void on_climate_state_(std::string state);            // hvac mode: off/heat/cool/dry/fan_only/auto
  void on_climate_action_(std::string action);          // hvac_action: heating/cooling/idle/off
  void on_climate_cur_temp_(std::string v);             // current_temperature
  void on_climate_target_temp_(std::string v);          // temperature (setpoint)
  void on_climate_fan_(std::string v);                  // fan_mode
  int light_index_for_entity_(const std::string &entity_id) const;
  int screen_index_for_entity_(const std::string &entity_id) const;

  // --- LVGL-task helpers ---
  static void tick_cb_(lv_timer_t *t);
  void tick_();               // 1 Hz, LVGL task
  static void flash_cb_(lv_timer_t *t);
  void flash_tick_();         // fast red/white flash in the final seconds, LVGL task
  void refresh_heater_ui_();  // LVGL task only — dial value/sub + arc
  void build_time_tile_(lv_obj_t *tile);     // LVGL task only
  void refresh_time_tile_();                 // LVGL task only — clock + temperature
  // Onboard BM8563 (PCF8563-compatible) RTC on the BSP's shared I2C bus.
  void seed_clock_from_rtc_();    // setup: read RTC -> seed system clock (main task)
  void write_rtc_from_system_();  // persist HA time back to the chip (main task)
  // Battery/power + network status for the clock-tile corner icons.
  void poll_power_status_();      // main task: read AXP2101 + network -> atomics
  void refresh_status_icons_();   // LVGL task: update the WiFi/battery icons
  static int batt_mv_to_pct_(int mv);  // rough LiPo SoC from resting mV
  void maybe_auto_revert_();                 // LVGL task only — idle -> clock/heater tile
  void maybe_screen_sleep_();                // LVGL task only — idle -> fade to black
  void maybe_deep_sleep_();                  // main task only — on battery + idle -> deep sleep
  void set_backlight_rail_(bool on);         // AXP2101 BLDO1 rail cut/restore (LVGL task)
  void set_cpu_freq_(int min_mhz, int max_mhz);  // DFS floor/ceiling via esp_pm (80-240 asleep / 240 awake)
  static void fade_step_cb_(void *var, int32_t v);  // lv_anim: set both eyelid heights
  // "Fighting sleep" close: the eyelids droop and snap back a couple of times
  // (each a keyframe) before finally shutting; then the backlight is cut. Wake
  // is a single, deliberate eyes-open sweep.
  void start_sleep_kf_(int idx);                    // LVGL task — run close keyframe #idx
  static void sleep_kf_done_cb_(lv_anim_t *a);      // lv_anim: advance to the next keyframe
  void finish_sleep_();                             // LVGL task — eyelids shut: cut rail + drop CPU
  static void wake_done_cb_(lv_anim_t *a);          // lv_anim: eyelids open -> hide overlays
  void build_screens_tile_(lv_obj_t *tile);  // LVGL task only
  void update_screen_visual_();              // LVGL task only
  void build_lights_tile_(lv_obj_t *tile);   // LVGL task only
  void refresh_lights_ui_();                 // LVGL task only
  void build_media_tile_(lv_obj_t *tile);    // LVGL task only
  void refresh_media_ui_();                  // LVGL task only
  void build_heater_tile_(lv_obj_t *tile);   // LVGL task only — iOS-timer style picker/countdown
  void build_climate_tile_(lv_obj_t *tile);  // LVGL task only — thermostat controls
  void refresh_climate_ui_();                // LVGL task only — setpoint/current/mode/fan + bg

  // --- config ---
  std::string run_script_{"script.patio_heater_run"};
  std::string stop_script_{"script.patio_heater_stop"};
  std::string timer_entity_{"timer.patio_heaters"};
  std::string temp_sensor_{"sensor.usl_environmental_temperature_3"};
  time::RealTimeClock *time_{nullptr};
  void *rtc_dev_{nullptr};            // i2c_master_dev_handle_t for the BM8563 (0x51)
  uint32_t last_rtc_write_ms_{0};     // throttles RTC write-back from HA time
  void *axp_dev_{nullptr};            // i2c_master_dev_handle_t for the AXP2101 (0x34)
  uint32_t last_status_poll_ms_{0};   // throttles battery/power/network polling
  bool deep_sleep_pending_{false};    // main task: latched once we ask to deep sleep
  Trigger<> deep_sleep_trigger_;      // fired to hand off to the deep_sleep component
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

  // tileview + bottom page-position dots (LVGL task only). The active tile set
  // + order is configured via YAML `tiles:`; only `num_tiles_` of these slots
  // are used, in the order given by `tile_order_`.
  lv_obj_t *tv_{nullptr};
  lv_obj_t *tiles_[MAX_TILES]{};
  lv_obj_t *page_dots_[MAX_TILES]{};
  uint8_t tile_order_[MAX_TILES]{};   // kind at each position (codegen order)
  int num_tiles_{0};
  // Named pointers for tiles other code jumps to (null when that tile isn't in
  // the configured set).
  lv_obj_t *time_tile_{nullptr};
  lv_obj_t *heater_tile_{nullptr};
  lv_obj_t *climate_tile_{nullptr};

  // Screen-sleep (burn-in guard): a full-screen transparent presser on the top
  // layer, revealed when the backlight is off so the first tap only wakes, plus
  // two black "eyelid" bars that close from top and bottom to sleep the screen.
  lv_obj_t *wake_eater_{nullptr};
  lv_obj_t *eyelid_top_{nullptr};
  lv_obj_t *eyelid_bottom_{nullptr};
  bool screen_asleep_{false};  // LVGL task only
  // millis() when the screen finished sleeping (backlight cut), 0 = awake.
  // Written on the LVGL task (finish_sleep_/wake_screen), read on the main task
  // (maybe_deep_sleep_) — hence atomic.
  std::atomic<uint32_t> screen_asleep_at_ms_{0};
  // Runtime idle timeouts (ms), adjustable from HA. Defaults match the historic
  // compile-time constants so the patio hub (which never sets them) is unchanged.
  // screen_sleep_ms_ = idle before the backlight cuts; deep_sleep_total_ms_ =
  // total idle before deep sleep (the "additional after screen off" delay is
  // computed as deep_sleep_total_ms_ - screen_sleep_ms_, floored at 0).
  std::atomic<uint32_t> screen_sleep_ms_{SCREEN_SLEEP_MS};
  std::atomic<uint32_t> deep_sleep_total_ms_{SCREEN_SLEEP_MS + DEEP_SLEEP_AFTER_SCREEN_MS};
  int sleep_kf_idx_{0};        // current "fighting sleep" close keyframe (LVGL task only)

  // clock/temperature tile widgets (LVGL task only)
  lv_obj_t *time_date_{nullptr};   // small header: weekday + date
  lv_obj_t *time_big_{nullptr};    // large 12-hour H:MM
  lv_obj_t *time_ampm_{nullptr};   // AM/PM
  lv_obj_t *temp_label_{nullptr};  // outside temperature (°C)
  lv_obj_t *status_wifi_{nullptr}; // "not connected" WiFi icon (clock tile, only when network down)
  lv_obj_t *status_batt_{nullptr}; // battery icon + % (clock tile, only when on battery)

  // screen tile widgets (LVGL task only)
  lv_obj_t *screen_btn_[NUM_SCREENS]{};
  lv_obj_t *screen_fill_[NUM_SCREENS]{};  // position-fill overlay (how far the blind is down)
  lv_obj_t *ctrl_up_{nullptr};
  lv_obj_t *ctrl_stop_{nullptr};
  lv_obj_t *ctrl_down_{nullptr};

  // light tile widgets (LVGL task only)
  lv_obj_t *light_slider_[NUM_LIGHTS]{};
  lv_obj_t *light_name_[NUM_LIGHTS]{};
  lv_obj_t *light_fill_[NUM_LIGHTS]{};   // manual peach fill layer (reaches track bottom)

  // media tile widgets (LVGL task only)
  lv_obj_t *media_title_lbl_{nullptr};   // now-playing / state text
  lv_obj_t *media_vol_slider_{nullptr};  // horizontal volume fader
  lv_obj_t *media_vol_pct_{nullptr};     // "NN%"
  lv_obj_t *media_playpause_lbl_{nullptr};  // symbol on the play/pause button

  // --- screen config / selection ---
  std::string screen_entity_[NUM_SCREENS];
  std::string screen_label_[NUM_SCREENS];
  bool screen_configured_[NUM_SCREENS]{};
  ScreenTap screen_tap_[NUM_SCREENS];
  bool screen_sel_[NUM_SCREENS]{};  // LVGL task only
  std::string screen_title_{"Screens"};
  int screen_layout_{0};  // 0=perimeter map, 1=horizontal row

  // --- light config ---
  std::string light_entity_[NUM_LIGHTS];
  std::string light_label_[NUM_LIGHTS];
  bool light_configured_[NUM_LIGHTS]{};
  LightCtrl light_ctrl_[NUM_LIGHTS];

  // --- media config ---
  std::string media_entity_;
  std::string media_label_{"Music"};
  bool media_configured_{false};

  // --- climate (thermostat) config ---
  std::string climate_entity_;
  std::string climate_label_{"Climate"};
  bool climate_configured_{false};
  float climate_min_{61.0f};
  float climate_max_{88.0f};
  float climate_step_{1.0f};
  bool climate_celsius_{false};
  // climate widgets (LVGL task only)
  lv_obj_t *climate_cur_lbl_{nullptr};    // "Now 76°"
  lv_obj_t *climate_set_lbl_{nullptr};    // big setpoint "68°"
  lv_obj_t *climate_mode_dd_{nullptr};    // mode selector dropdown (Off/Cool/Heat/Auto)
  lv_obj_t *climate_fan_dd_{nullptr};     // fan selector dropdown (Auto/Low/Med/High)
  // climate cross-task state (main task writes from HA, LVGL task reads). Temps
  // are stored *10 to keep half-degree steps in integer atomics; -10000 == unknown.
  std::atomic<int> climate_mode_{0};       // index into the full mode table (see cpp)
  std::atomic<int> climate_action_{0};     // 0 off/idle,1 heating,2 cooling,3 drying,4 fan
  std::atomic<int> climate_cur_x10_{-10000};    // current temperature *10
  std::atomic<int> climate_target_x10_{-10000}; // setpoint *10
  std::atomic<int> climate_fan_{0};        // index into the fan table (see cpp)
  std::atomic<bool> climate_ui_dirty_{true};
  // pending intents (LVGL task -> main task); sentinels mean "nothing pending".
  std::atomic<int> pending_climate_target_x10_{-10000};  // absolute setpoint *10 to send
  std::atomic<int> pending_climate_mode_{-1};            // mode index to send
  std::atomic<int> pending_climate_fan_{-1};             // fan index to send

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
  // Per-cover position feedback (Lutron etc.). 0=closed/down, 100=open/up,
  // -1=unknown (e.g. Somfy RTS, which never reports position -> never disables).
  std::atomic<int> screen_pos_[NUM_SCREENS];
  std::atomic<bool> screen_ui_dirty_{false};     // HA position changed -> refresh visuals

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

  // clock tile: power/network status for the top-corner icons. Polled on the
  // main task from the AXP2101 + network state, drawn on the LVGL task.
  std::atomic<bool> wifi_up_{true};       // network (WiFi) currently connected
  std::atomic<bool> on_battery_{false};   // running on battery (no VBUS)
  std::atomic<int> batt_pct_{-1};         // battery charge 0..100, -1 = unknown

  // media tile: Sonos amp state (HA -> UI) + pending intents (UI -> HA).
  std::atomic<int> media_state_{0};       // 0 idle/stopped, 1 playing, 2 paused
  std::atomic<int> media_vol_{-1};        // 0..100 from HA, -1 unknown
  std::atomic<bool> media_ui_dirty_{true};// HA changed -> refresh the tile
  std::atomic<int> pending_media_vol_{-1};// 0..100 to send, -1 none
  std::atomic<int> pending_media_cmd_{0}; // 0 none, 1 play/pause, 2 next, 3 prev
  // Now-playing title (API task writes, LVGL task reads) guarded by a spinlock,
  // since std::string isn't safe to share across tasks lock-free.
  portMUX_TYPE media_title_mux_ = portMUX_INITIALIZER_UNLOCKED;
  char media_title_[64]{};
};

}  // namespace patio_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
