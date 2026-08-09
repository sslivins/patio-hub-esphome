#include "patio_ui.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "png_uncompressed.h"

#include "esphome/core/application.h"

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
static void ev_up(lv_event_t *e) {  // screen goes up = open
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_cover_action(1);
}
static void ev_down(lv_event_t *e) {  // screen comes down = close
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_cover_action(2);
}
static void ev_cover_stop(lv_event_t *e) {
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_cover_action(3);
}
static void ev_tile_scroll(lv_event_t *e) {  // active tile changed -> update dots
  static_cast<PatioUI *>(lv_event_get_user_data(e))->update_page_dots_();
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
//
// `orient` controls where the roller/valance bar sits, so the side screens
// (mounted on the side walls, seen edge-on) read as a different orientation
// from the rear screens (on the far wall, facing you):
//   SCR_REAR  -> horizontal bar across the top   (shade rolls down)
//   SCR_LEFT  -> vertical bar down the left edge  (side wall, left)
//   SCR_RIGHT -> vertical bar down the right edge (side wall, right)
enum ScreenOrient { SCR_REAR, SCR_LEFT, SCR_RIGHT };

static lv_obj_t *make_screen_button(lv_obj_t *parent, void *user, ScreenOrient orient) {
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
  lv_obj_set_size(pane, LV_PCT(72), LV_PCT(72));
  lv_obj_center(pane);
  lv_obj_set_style_bg_color(pane, lv_color_hex(0x2E6B7A), 0);
  lv_obj_set_style_bg_opa(pane, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pane, 3, 0);
  lv_obj_clear_flag(pane, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(pane, LV_OBJ_FLAG_EVENT_BUBBLE);

  // roller/valance bar — its side signals the screen's mounting orientation.
  lv_obj_t *roller = lv_obj_create(pane);
  lv_obj_remove_style_all(roller);
  if (orient == SCR_REAR) {
    lv_obj_set_size(roller, LV_PCT(100), 7);
    lv_obj_align(roller, LV_ALIGN_BOTTOM_MID, 0, 0);
  } else if (orient == SCR_LEFT) {
    lv_obj_set_size(roller, 7, LV_PCT(100));
    lv_obj_align(roller, LV_ALIGN_LEFT_MID, 0, 0);
  } else {  // SCR_RIGHT
    lv_obj_set_size(roller, 7, LV_PCT(100));
    lv_obj_align(roller, LV_ALIGN_RIGHT_MID, 0, 0);
  }
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
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);  // replaced by page dots below
  this->tv_ = tv;

  lv_obj_t *t_heater = lv_tileview_add_tile(tv, 0, 0, LV_DIR_HOR);
  lv_obj_t *t_lights = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
  lv_obj_t *t_screens = lv_tileview_add_tile(tv, 2, 0, LV_DIR_HOR);
  this->tiles_[0] = t_heater;
  this->tiles_[1] = t_lights;
  this->tiles_[2] = t_screens;

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

  // Bottom page-position dots (replaces the tileview scroll line). Live on the
  // screen so they float over every tile; updated when the active tile changes.
  lv_obj_t *dots = lv_obj_create(scr);
  lv_obj_remove_style_all(dots);
  lv_obj_set_size(dots, 70, 14);
  lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -3);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots, 9, 0);
  lv_obj_clear_flag(dots, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *d = lv_obj_create(dots);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_40, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    this->page_dots_[i] = d;
  }
  lv_obj_add_event_cb(tv, ev_tile_scroll, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(tv, ev_tile_scroll, LV_EVENT_SCROLL_END, this);
  this->update_page_dots_();

  // 1 Hz refresh/countdown driver (LVGL task)
  this->tick_timer_ = lv_timer_create(PatioUI::tick_cb_, 1000, this);
  this->refresh_heater_label_();
}

// Highlights the dot for the currently-active tile (bright), dims the others.
void PatioUI::update_page_dots_() {
  if (this->tv_ == nullptr)
    return;
  lv_obj_t *active = lv_tileview_get_tile_active(this->tv_);
  for (int i = 0; i < 3; i++) {
    if (this->page_dots_[i] == nullptr)
      continue;
    bool on = (this->tiles_[i] == active);
    lv_obj_set_style_bg_opa(this->page_dots_[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
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

  // Left (slot 0) / Right (slot 1): thin, tall side tiles on the edges, with a
  // vertical valance bar so they read as side-wall-mounted (different from rear).
  this->screen_btn_[0] = make_screen_button(tile, &this->screen_tap_[0], SCR_LEFT);
  lv_obj_set_size(this->screen_btn_[0], 52, 100);
  lv_obj_align(this->screen_btn_[0], LV_ALIGN_TOP_LEFT, 8, 32);

  this->screen_btn_[1] = make_screen_button(tile, &this->screen_tap_[1], SCR_RIGHT);
  lv_obj_set_size(this->screen_btn_[1], 52, 100);
  lv_obj_align(this->screen_btn_[1], LV_ALIGN_TOP_RIGHT, -8, 32);

  // Rear Left (slot 2) / Rear Right (slot 3): the two side-by-side screens on the
  // far wall behind the viewer — a centered landscape pair (horizontal valance),
  // sitting just above the control bar.
  this->screen_btn_[2] = make_screen_button(tile, &this->screen_tap_[2], SCR_REAR);
  lv_obj_set_size(this->screen_btn_[2], 88, 52);
  lv_obj_align(this->screen_btn_[2], LV_ALIGN_BOTTOM_MID, -47, -74);

  this->screen_btn_[3] = make_screen_button(tile, &this->screen_tap_[3], SCR_REAR);
  lv_obj_set_size(this->screen_btn_[3], 88, 52);
  lv_obj_align(this->screen_btn_[3], LV_ALIGN_BOTTOM_MID, 47, -74);

  // Control bar: up (open) / stop / down (close) — acts on the selection.
  // Somfy RTS has no feedback, so these are momentary commands, not toggles.
  // Disabled + dimmed when nothing is selected (see update_screen_visual_).
  lv_obj_t *bar = lv_obj_create(tile);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 320, 50);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  this->ctrl_up_ = make_btn(bar, LV_SYMBOL_UP, ev_up, this);
  lv_obj_set_size(this->ctrl_up_, 98, 44);
  this->ctrl_stop_ = make_btn(bar, LV_SYMBOL_STOP, ev_cover_stop, this);
  lv_obj_set_size(this->ctrl_stop_, 98, 44);
  this->ctrl_down_ = make_btn(bar, LV_SYMBOL_DOWN, ev_down, this);
  lv_obj_set_size(this->ctrl_down_, 98, 44);

  // Dimmed look for the disabled state (nothing selected).
  lv_obj_t *ctrls[3] = {this->ctrl_up_, this->ctrl_stop_, this->ctrl_down_};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_opa(ctrls[i], LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(lv_obj_get_child(ctrls[i], 0), LV_OPA_30, LV_STATE_DISABLED);
  }

  // Default: everything selected (tap a tile to exclude it — no separate ALL).
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (this->screen_configured_[i])
      this->screen_sel_[i] = true;
    else
      lv_obj_add_flag(this->screen_btn_[i], LV_OBJ_FLAG_HIDDEN);
  }

  this->update_screen_visual_();
}

// ---------------- selection highlight + control state (LVGL task) ----------------
void PatioUI::update_screen_visual_() {
  bool any_sel = false;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (!this->screen_configured_[i])
      continue;
    lv_obj_t *b = this->screen_btn_[i];
    if (b == nullptr)
      continue;
    if (this->screen_sel_[i]) {
      any_sel = true;
      lv_obj_set_style_border_color(b, COL_SEL, 0);
      lv_obj_set_style_border_width(b, 4, 0);
      lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
      lv_obj_set_style_opa(b, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_border_color(b, lv_color_white(), 0);
      lv_obj_set_style_border_width(b, 1, 0);
      lv_obj_set_style_border_opa(b, LV_OPA_40, 0);
      lv_obj_set_style_opa(b, LV_OPA_50, 0);  // dim deselected screens
    }
  }

  // Control buttons are only actionable when at least one screen is selected.
  lv_obj_t *ctrls[3] = {this->ctrl_up_, this->ctrl_stop_, this->ctrl_down_};
  for (int i = 0; i < 3; i++) {
    if (ctrls[i] == nullptr)
      continue;
    if (any_sel) {
      lv_obj_clear_state(ctrls[i], LV_STATE_DISABLED);
      lv_obj_add_flag(ctrls[i], LV_OBJ_FLAG_CLICKABLE);
    } else {
      lv_obj_add_state(ctrls[i], LV_STATE_DISABLED);
      lv_obj_clear_flag(ctrls[i], LV_OBJ_FLAG_CLICKABLE);
    }
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
// ---------------- screenshot HTTP endpoint ----------------
// Grab the live LVGL framebuffer and stream it as an uncompressed PNG, mirroring
// the arctic-controller /api/screenshot mechanism. Lets us (and CI) see exactly
// what the panel is rendering without a physical photo. Served on port 8080:
//   curl http://<device-ip>:8080/screenshot -o shot.png
static esp_err_t png_http_write(void *ctx, const void *buf, size_t len) {
  return httpd_resp_send_chunk((httpd_req_t *) ctx, (const char *) buf, len);
}

static esp_err_t screenshot_get_handler(httpd_req_t *req) {
  bsp_display_lock(0);
  lv_obj_t *screen = lv_screen_active();
  lv_obj_update_layout(screen);
  int32_t w = lv_obj_get_width(screen);
  int32_t h = lv_obj_get_height(screen);
  bsp_display_unlock();

  uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB888);
  uint32_t buf_size = stride * h;
  void *pixel_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
  if (pixel_buf == nullptr) {
    ESP_LOGE(TAG, "screenshot: OOM (%lu bytes)", (unsigned long) buf_size);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_OK;
  }

  lv_draw_buf_t snapshot;
  lv_draw_buf_init(&snapshot, w, h, LV_COLOR_FORMAT_RGB888, stride, pixel_buf, buf_size);

  bsp_display_lock(0);
  screen = lv_screen_active();
  lv_result_t snap_res = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB888, &snapshot);
  bsp_display_unlock();

  if (snap_res != LV_RESULT_OK) {
    ESP_LOGE(TAG, "screenshot: snapshot failed");
    heap_caps_free(pixel_buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Snapshot failed");
    return ESP_OK;
  }

  // LVGL RGB888 packs bytes B,G,R; swap to R,G,B and drop stride padding in place.
  uint8_t *dst = (uint8_t *) pixel_buf;
  const uint8_t *src_row = (const uint8_t *) pixel_buf;
  for (int32_t y = 0; y < h; y++) {
    const uint8_t *s = src_row;
    for (int32_t x = 0; x < w; x++) {
      uint8_t b = s[0], g = s[1], r = s[2];
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
      dst += 3;
      s += 3;
    }
    src_row += stride;
  }

  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.png\"");

  int64_t t0 = esp_timer_get_time();
  esp_err_t ret = png_encode_uncompressed_rgb888((const uint8_t *) pixel_buf, w, h, png_http_write, req);
  int64_t encode_ms = (esp_timer_get_time() - t0) / 1000;
  heap_caps_free(pixel_buf);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "screenshot: PNG stream failed: %s", esp_err_to_name(ret));
    return ret;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  ESP_LOGI(TAG, "screenshot streamed: %ldx%ld in %ld ms", (long) w, (long) h, (long) encode_ms);
  return ESP_OK;
}

// Lightweight build/health probe so we can confirm exactly which firmware image
// is actually running (crash-rollback can silently revert to a prior image).
// The app ELF SHA-256 uniquely identifies the build; date/time come from the
// same esp_app_desc baked in at link time.
//   curl http://<device-ip>:8080/status
static esp_err_t status_get_handler(httpd_req_t *req) {
  const esp_app_desc_t *d = esp_app_get_description();
  char sha[17] = {0};
  for (int i = 0; i < 8; i++)
    snprintf(sha + i * 2, 3, "%02x", d->app_elf_sha256[i]);

  char build_time[App.BUILD_TIME_STR_SIZE] = {0};
  App.get_build_time_string(build_time);

  char body[512];
  int n = snprintf(body, sizeof(body),
                   "{\"project\":\"%s\",\"esphome\":\"%s\",\"compiled\":\"%s\","
                   "\"idf\":\"%s\",\"elf_sha256\":\"%s\","
                   "\"uptime_s\":%lld,\"free_heap\":%u,\"free_psram\":%u}",
                   d->project_name, d->version, build_time, d->idf_ver, sha,
                   (long long) (esp_timer_get_time() / 1000000),
                   (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, body, n);
  return ESP_OK;
}

void PatioUI::start_screenshot_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 8080;
  cfg.ctrl_port = 32080;  // avoid clashing with any other httpd control socket
  cfg.stack_size = 8192;  // software-render snapshot needs more than the 4KB default
  cfg.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    ESP_LOGW(TAG, "screenshot server failed to start");
    return;
  }
  httpd_uri_t uri = {};
  uri.uri = "/screenshot";
  uri.method = HTTP_GET;
  uri.handler = screenshot_get_handler;
  httpd_register_uri_handler(server, &uri);

  httpd_uri_t status_uri = {};
  status_uri.uri = "/status";
  status_uri.method = HTTP_GET;
  status_uri.handler = status_get_handler;
  httpd_register_uri_handler(server, &status_uri);

  this->screenshot_httpd_ = server;
  ESP_LOGI(TAG, "http endpoints on :8080  /screenshot  /status");
}

void PatioUI::setup() {
  ESP_LOGI(TAG, "bringing up Core2 display + LVGL");

  // The BSP (our esp-bsp fork) initialises the new-i2c bus, powers the AXP2101
  // LCD rails and pulses the ALDO2 LCD/touch reset itself, then brings up the
  // esp_lcd SPI panel + FT6336 touch + esp_lvgl_port DMA flush pipeline.
  //
  // Tearing note (Core2 / ESP32-classic): the BSP default is a 50-row double
  // DMA buffer, so a full-screen change (swiping tiles) is flushed as ~5
  // horizontal bands that land a few ms apart — the "stripes" seen while
  // scrolling. The clean fix is a full-frame render buffer, but on this SoC it
  // is not achievable via esp_lvgl_port: SOC_PSRAM_DMA_CAPABLE == 0 (SPI DMA
  // cannot read PSRAM), and a full 320x240xRGB565 frame (150 KB) does not fit
  // in internal DMA RAM. A full-frame PSRAM buffer therefore faults in LVGL
  // wait_for_flushing (the flush DMAs straight from the PSRAM px_map). This is
  // fixed for free on the CoreS3 (ESP32-S3, SOC_PSRAM_DMA_CAPABLE == 1), where
  // buff_dma+buff_spiram is supported — defer the tear-free path to that board.
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

  // Live screen capture endpoint (uncompressed PNG on :8080/screenshot).
  this->start_screenshot_server_();
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
