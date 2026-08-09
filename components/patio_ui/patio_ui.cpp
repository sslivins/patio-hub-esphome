#include "patio_ui.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace patio_ui {

static const char *const TAG = "patio_ui";

// --- palette (matches the native PoC) ---
#define COL_HEATER lv_color_hex(0x8A4B1E)
#define COL_LIGHTS lv_color_hex(0x7A6A1E)
#define COL_SCREENS lv_color_hex(0x1E5A6E)
#define COL_BTN lv_color_hex(0x2E2E2E)
#define COL_SCREEN_TILE lv_color_hex(0x14424F)
#define COL_SEL lv_color_hex(0xFFD54A)

// event-callback trampolines (run on the LVGL task)
static void ev_start(lv_event_t *e) {
  auto *self = static_cast<PatioUI *>(lv_event_get_user_data(e));
  self->request_start();
}
static void ev_stop(lv_event_t *e) {
  auto *self = static_cast<PatioUI *>(lv_event_get_user_data(e));
  self->request_stop();
}
static void ev_minus(lv_event_t *e) {
  auto *self = static_cast<PatioUI *>(lv_event_get_user_data(e));
  self->adjust_setpoint(-5);
}
static void ev_plus(lv_event_t *e) {
  auto *self = static_cast<PatioUI *>(lv_event_get_user_data(e));
  self->adjust_setpoint(+5);
}
static void ev_screen_tap(lv_event_t *e) {
  auto *t = static_cast<PatioUI::ScreenTap *>(lv_event_get_user_data(e));
  t->self->toggle_screen_sel(t->idx);
}
static void ev_all(lv_event_t *e) {
  auto *self = static_cast<PatioUI *>(lv_event_get_user_data(e));
  self->toggle_all_sel();
}
static void ev_up(lv_event_t *e) {  // screen goes up = open
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_cover_action(1);
}
static void ev_down(lv_event_t *e) {  // screen comes down = close
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_cover_action(2);
}
static void ev_cover_stop(lv_event_t *e) {
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_cover_action(3);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *user) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, 88, 46);
  lv_obj_set_style_bg_color(btn, COL_BTN, 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_center(lbl);
  if (cb != nullptr)
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
  return btn;
}

// A perimeter "screen" tile drawn as a little roller-shade icon (no text —
// its on-screen position tells you which physical screen it is). Tapping it
// toggles selection (ev_screen_tap via the ScreenTap user data). Somfy RTS
// screens report no real state, so nothing dynamic is shown here.
static lv_obj_t *make_screen_button(lv_obj_t *parent, void *user) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_style_bg_color(btn, COL_SCREEN_TILE, 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_border_color(btn, lv_color_white(), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);

  // window pane (inset lighter rect)
  lv_obj_t *pane = lv_obj_create(btn);
  lv_obj_remove_style_all(pane);
  lv_obj_set_size(pane, LV_PCT(72), LV_PCT(70));
  lv_obj_center(pane);
  lv_obj_set_style_bg_color(pane, lv_color_hex(0x2E6B7A), 0);
  lv_obj_set_style_bg_opa(pane, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pane, 3, 0);
  lv_obj_clear_flag(pane, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(pane, LV_OBJ_FLAG_EVENT_BUBBLE);

  // roller/valance bar across the top of the pane
  lv_obj_t *roller = lv_obj_create(pane);
  lv_obj_remove_style_all(roller);
  lv_obj_set_size(roller, LV_PCT(100), 7);
  lv_obj_align(roller, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(roller, lv_color_hex(0x0E3540), 0);
  lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(roller, 3, 0);
  lv_obj_clear_flag(roller, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(roller, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_add_event_cb(btn, ev_screen_tap, LV_EVENT_CLICKED, user);
  return btn;
}

// A simple static tile: colored bg, title, big value, up to 3 (dummy) buttons.
static void build_static_tile(lv_obj_t *tile, lv_color_t bg, const char *title, const char *big, const char *b1,
                              const char *b2, const char *b3) {
  lv_obj_set_style_bg_color(tile, bg, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tile, 10, 0);

  lv_obj_t *t = lv_label_create(tile);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);

  lv_obj_t *v = lv_label_create(tile);
  lv_label_set_text(v, big);
  lv_obj_set_style_text_color(v, lv_color_white(), 0);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_48, 0);

  lv_obj_t *row = lv_obj_create(tile);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 300, 52);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  make_btn(row, b1, nullptr, nullptr);
  make_btn(row, b2, nullptr, nullptr);
  if (b3 != nullptr)
    make_btn(row, b3, nullptr, nullptr);
}

void PatioUI::build_ui_() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  lv_obj_t *tv = lv_tileview_create(scr);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);

  lv_obj_t *t_heater = lv_tileview_add_tile(tv, 0, 0, LV_DIR_HOR);
  lv_obj_t *t_lights = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
  lv_obj_t *t_screens = lv_tileview_add_tile(tv, 2, 0, LV_DIR_HOR);

  // --- heater tile (live, wired to HA) ---
  lv_obj_set_style_bg_color(t_heater, COL_HEATER, 0);
  lv_obj_set_style_bg_opa(t_heater, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(t_heater, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(t_heater, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(t_heater, 10, 0);

  lv_obj_t *ht = lv_label_create(t_heater);
  lv_label_set_text(ht, "Heater");
  lv_obj_set_style_text_color(ht, lv_color_white(), 0);
  lv_obj_set_style_text_font(ht, &lv_font_montserrat_28, 0);

  this->heater_value_ = lv_label_create(t_heater);
  lv_obj_set_style_text_color(this->heater_value_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->heater_value_, &lv_font_montserrat_48, 0);
  lv_label_set_text(this->heater_value_, "--");

  lv_obj_t *row = lv_obj_create(t_heater);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 308, 52);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  make_btn(row, "-5", ev_minus, this);
  make_btn(row, "+5", ev_plus, this);
  make_btn(row, "Start", ev_start, this);
  make_btn(row, "Stop", ev_stop, this);

  // --- lights tile (static placeholder for now) ---
  build_static_tile(t_lights, COL_LIGHTS, "Lights", "75%", "Off", "Dim", "On");

  // --- screens tile (live perimeter map, wired to HA covers) ---
  this->build_screens_tile_(t_screens);

  // 1 Hz refresh/countdown driver (LVGL task)
  this->tick_timer_ = lv_timer_create(PatioUI::tick_cb_, 1000, this);
  this->refresh_heater_label_();
}

// ---------------- public request API (called from LVGL task) ----------------
void PatioUI::request_start() {
  int m = this->setpoint_minutes_.load();
  this->pending_start_.store(m);
}
void PatioUI::request_stop() { this->pending_stop_.store(true); }
void PatioUI::adjust_setpoint(int delta) {
  int m = this->setpoint_minutes_.load() + delta;
  if (m < this->min_minutes_)
    m = this->min_minutes_;
  if (m > this->max_minutes_)
    m = this->max_minutes_;
  this->setpoint_minutes_.store(m);
  this->label_dirty_.store(true);
}

// ---------------- LVGL-task label rendering ----------------
void PatioUI::tick_cb_(lv_timer_t *t) { static_cast<PatioUI *>(lv_timer_get_user_data(t))->tick_(); }

void PatioUI::tick_() {
  // local 1 Hz countdown while active
  if (this->active_.load()) {
    int s = this->countdown_secs_.load();
    if (s > 0) {
      this->countdown_secs_.store(s - 1);
    }
    this->label_dirty_.store(true);
  }
  if (this->label_dirty_.exchange(false))
    this->refresh_heater_label_();
}

void PatioUI::refresh_heater_label_() {
  if (this->heater_value_ == nullptr)
    return;
  char buf[16];
  if (this->active_.load()) {
    int s = this->countdown_secs_.load();
    if (s < 0)
      s = 0;
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0)
      snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else
      snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
  } else {
    snprintf(buf, sizeof(buf), "%d min", this->setpoint_minutes_.load());
  }
  lv_label_set_text(this->heater_value_, buf);
}

// ---------------- screens tile (LVGL task) ----------------
void PatioUI::build_screens_tile_(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, COL_SCREENS, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);

  lv_obj_t *hdr = lv_label_create(tile);
  lv_label_set_text(hdr, "Screens");
  lv_obj_set_style_text_color(hdr, lv_color_white(), 0);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);

  for (int i = 0; i < NUM_SCREENS; i++) {
    this->screen_tap_[i].self = this;
    this->screen_tap_[i].idx = i;
  }

  // Left (slot 0) and Right (slot 1): tall side tiles.
  this->screen_btn_[0] = make_screen_button(tile, &this->screen_tap_[0]);
  lv_obj_set_size(this->screen_btn_[0], 72, 118);
  lv_obj_align(this->screen_btn_[0], LV_ALIGN_TOP_LEFT, 6, 30);

  this->screen_btn_[1] = make_screen_button(tile, &this->screen_tap_[1]);
  lv_obj_set_size(this->screen_btn_[1], 72, 118);
  lv_obj_align(this->screen_btn_[1], LV_ALIGN_TOP_RIGHT, -6, 30);

  // ALL: center button (select/deselect everything).
  this->all_btn_ = lv_button_create(tile);
  lv_obj_set_size(this->all_btn_, 96, 60);
  lv_obj_align(this->all_btn_, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_bg_color(this->all_btn_, COL_SCREEN_TILE, 0);
  lv_obj_set_style_radius(this->all_btn_, 8, 0);
  lv_obj_set_style_border_color(this->all_btn_, lv_color_white(), 0);
  lv_obj_set_style_border_width(this->all_btn_, 1, 0);
  lv_obj_set_style_border_opa(this->all_btn_, LV_OPA_40, 0);
  lv_obj_t *all_lbl = lv_label_create(this->all_btn_);
  lv_label_set_text(all_lbl, "ALL");
  lv_obj_set_style_text_font(all_lbl, &lv_font_montserrat_28, 0);
  lv_obj_center(all_lbl);
  lv_obj_add_event_cb(this->all_btn_, ev_all, LV_EVENT_CLICKED, this);

  // Rear Left (slot 2) / Rear Right (slot 3): the two side-by-side screens
  // behind the viewer, shown as a bottom-center pair.
  this->screen_btn_[2] = make_screen_button(tile, &this->screen_tap_[2]);
  lv_obj_set_size(this->screen_btn_[2], 72, 42);
  lv_obj_align(this->screen_btn_[2], LV_ALIGN_TOP_MID, -42, 100);

  this->screen_btn_[3] = make_screen_button(tile, &this->screen_tap_[3]);
  lv_obj_set_size(this->screen_btn_[3], 72, 42);
  lv_obj_align(this->screen_btn_[3], LV_ALIGN_TOP_MID, 42, 100);

  // Control bar: up (open) / stop / down (close) — acts on the selection.
  // Somfy RTS has no feedback, so these are momentary commands, not toggles.
  lv_obj_t *bar = lv_obj_create(tile);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 320, 50);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *b_up = make_btn(bar, LV_SYMBOL_UP, ev_up, this);
  lv_obj_set_size(b_up, 98, 44);
  lv_obj_t *b_stop = make_btn(bar, LV_SYMBOL_STOP, ev_cover_stop, this);
  lv_obj_set_size(b_stop, 98, 44);
  lv_obj_t *b_down = make_btn(bar, LV_SYMBOL_DOWN, ev_down, this);
  lv_obj_set_size(b_down, 98, 44);

  // Hide any unconfigured slots.
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (!this->screen_configured_[i])
      lv_obj_add_flag(this->screen_btn_[i], LV_OBJ_FLAG_HIDDEN);
  }

  this->update_screen_visual_();
}

// ---------------- selection highlight (LVGL task) ----------------
void PatioUI::update_screen_visual_() {
  bool all_sel = true;
  bool any_cfg = false;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (!this->screen_configured_[i])
      continue;
    any_cfg = true;
    if (!this->screen_sel_[i])
      all_sel = false;
    lv_obj_t *b = this->screen_btn_[i];
    if (b == nullptr)
      continue;
    if (this->screen_sel_[i]) {
      lv_obj_set_style_border_color(b, COL_SEL, 0);
      lv_obj_set_style_border_width(b, 4, 0);
      lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_border_color(b, lv_color_white(), 0);
      lv_obj_set_style_border_width(b, 1, 0);
      lv_obj_set_style_border_opa(b, LV_OPA_40, 0);
    }
  }
  if (this->all_btn_ != nullptr) {
    bool on = any_cfg && all_sel;
    lv_obj_set_style_border_color(this->all_btn_, on ? COL_SEL : lv_color_white(), 0);
    lv_obj_set_style_border_width(this->all_btn_, on ? 4 : 1, 0);
    lv_obj_set_style_border_opa(this->all_btn_, on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
}

// ---------------- screen intents (LVGL task) ----------------
void PatioUI::add_screen(int slot, const std::string &entity, const std::string &label) {
  if (slot < 0 || slot >= NUM_SCREENS)
    return;
  this->screen_entity_[slot] = entity;
  this->screen_label_[slot] = label;
  this->screen_configured_[slot] = true;
}

void PatioUI::toggle_screen_sel(int idx) {
  if (idx < 0 || idx >= NUM_SCREENS || !this->screen_configured_[idx])
    return;
  this->screen_sel_[idx] = !this->screen_sel_[idx];
  this->update_screen_visual_();
}

void PatioUI::toggle_all_sel() {
  bool all_sel = true;
  bool any_cfg = false;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (!this->screen_configured_[i])
      continue;
    any_cfg = true;
    if (!this->screen_sel_[i])
      all_sel = false;
  }
  bool target = any_cfg ? !all_sel : false;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (this->screen_configured_[i])
      this->screen_sel_[i] = target;
  }
  this->update_screen_visual_();
}

void PatioUI::request_cover_action(int action) {
  unsigned mask = 0;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (this->screen_configured_[i] && this->screen_sel_[i])
      mask |= (1u << i);
  }
  if (mask == 0)
    return;  // nothing selected -> no-op
  this->pending_cover_mask_.store(mask);
  this->pending_cover_action_.store(action);
}

// ---------------- Home Assistant state callbacks (main/API task) ----------------
void PatioUI::on_timer_state_(std::string state) {
  bool active = (state == "active");
  this->active_.store(active);
  if (!active)
    this->countdown_secs_.store(-1);
  this->label_dirty_.store(true);
  ESP_LOGD(TAG, "timer state: %s", state.c_str());
}

void PatioUI::on_timer_remaining_(std::string remaining) {
  // "H:MM:SS" -> seconds
  int h = 0, m = 0, s = 0;
  if (sscanf(remaining.c_str(), "%d:%d:%d", &h, &m, &s) == 3) {
    this->countdown_secs_.store(h * 3600 + m * 60 + s);
    this->label_dirty_.store(true);
    ESP_LOGD(TAG, "timer remaining: %s (%d s)", remaining.c_str(), h * 3600 + m * 60 + s);
  }
}

// ---------------- ESPHome Component ----------------
void PatioUI::setup() {
  ESP_LOGI(TAG, "bringing up Core2 display + LVGL");

  // The BSP (our esp-bsp fork) initialises the new-i2c bus, powers the AXP2101
  // LCD rails and pulses the ALDO2 LCD/touch reset itself, then brings up the
  // esp_lcd SPI panel + FT6336 touch + esp_lvgl_port DMA flush pipeline.
  lv_display_t *disp = bsp_display_start();
  if (disp == nullptr) {
    ESP_LOGE(TAG, "bsp_display_start failed");
    this->mark_failed();
    return;
  }
  bsp_display_backlight_on();
  bsp_display_brightness_set(80);

  bsp_display_lock(0);
  this->build_ui_();
  bsp_display_unlock();

  // Subscribe to the HA timer (api is a hard dependency, so global_api_server is up).
  this->subscribe_homeassistant_state(&PatioUI::on_timer_state_, this->timer_entity_);
  this->subscribe_homeassistant_state(&PatioUI::on_timer_remaining_, this->timer_entity_, "remaining");

  // Screens are Somfy RTS (command-only, no reliable state feedback), so we
  // don't subscribe to their state — the tiles are selectors + momentary
  // up/stop/down commands.

  ESP_LOGI(TAG, "UI up; heater tile bound to %s", this->timer_entity_.c_str());
}

void PatioUI::loop() {
  // Drain button intents on the main task, where the native API lives.
  int start_m = this->pending_start_.exchange(-1);
  if (start_m >= 0) {
    ESP_LOGI(TAG, "heater start -> %s (%d min)", this->run_script_.c_str(), start_m);
    this->call_homeassistant_service(this->run_script_, {{"minutes", std::to_string(start_m)}});
  }
  if (this->pending_stop_.exchange(false)) {
    ESP_LOGI(TAG, "heater stop -> %s", this->stop_script_.c_str());
    this->call_homeassistant_service(this->stop_script_);
  }

  // Drain a pending cover action against the selected screens.
  int act = this->pending_cover_action_.exchange(0);
  if (act != 0) {
    unsigned mask = this->pending_cover_mask_.load();
    const char *svc = (act == 1) ? "cover.open_cover" : (act == 2) ? "cover.close_cover" : "cover.stop_cover";
    for (int i = 0; i < NUM_SCREENS; i++) {
      if (((mask >> i) & 1u) && this->screen_configured_[i]) {
        ESP_LOGI(TAG, "%s -> %s", svc, this->screen_entity_[i].c_str());
        this->call_homeassistant_service(svc, {{"entity_id", this->screen_entity_[i]}});
      }
    }
  }
}

void PatioUI::dump_config() {
  ESP_LOGCONFIG(TAG, "Patio UI:");
  ESP_LOGCONFIG(TAG, "  timer entity: %s", this->timer_entity_.c_str());
  ESP_LOGCONFIG(TAG, "  run script:   %s", this->run_script_.c_str());
  ESP_LOGCONFIG(TAG, "  stop script:  %s", this->stop_script_.c_str());
  ESP_LOGCONFIG(TAG, "  minutes range: %d..%d", this->min_minutes_, this->max_minutes_);
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (this->screen_configured_[i])
      ESP_LOGCONFIG(TAG, "  screen[%d] %-9s -> %s", i, this->screen_label_[i].c_str(),
                    this->screen_entity_[i].c_str());
  }
}

}  // namespace patio_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
