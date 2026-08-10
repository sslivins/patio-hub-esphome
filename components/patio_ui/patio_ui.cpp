#include "patio_ui.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "png_uncompressed.h"

#include "esphome/core/application.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace patio_ui {

static const char *const TAG = "patio_ui";

// 96px digit+colon font (0-9 and ':') generated with lv_font_conv; compiled as
// C in patio_font_countdown.c, so it needs C linkage here.
extern "C" const lv_font_t patio_font_countdown;

// --- palette (matches the native PoC) ---
#define COL_HEATER lv_color_hex(0x8A4B1E)
#define COL_LIGHTS lv_color_hex(0x7A6A1E)
#define COL_SCREENS lv_color_hex(0x1E5A6E)
#define COL_TIME lv_color_hex(0x1B2A4A)   // clock/temperature lead tile (deep slate blue)
#define COL_MEDIA lv_color_hex(0x432A5E)  // deck media tile (deep purple)

// Heater "nearing expiry" fade: the tile background shifts from the normal
// brown, through amber, to red over the final EXPIRY_FADE_SECS of the run.
#define COL_HEATER_AMBER lv_color_hex(0xC46A12)
#define COL_HEATER_RED lv_color_hex(0xB01E10)
static const int EXPIRY_FADE_SECS = 300;  // start fading in the last 5 minutes
static const int EXPIRY_FLASH_SECS = 10;  // hard red/white flash in the last 10 s
#define COL_FLASH lv_color_hex(0xF5121A)   // vivid red for the flash

// Linear blend of two RGB colours. f=0 -> a, f=1 -> b.
static lv_color_t lerp_color(lv_color_t a, lv_color_t b, float f) {
  if (f < 0.0f)
    f = 0.0f;
  if (f > 1.0f)
    f = 1.0f;
  uint8_t r = (uint8_t) (a.red + (b.red - a.red) * f);
  uint8_t g = (uint8_t) (a.green + (b.green - a.green) * f);
  uint8_t bl = (uint8_t) (a.blue + (b.blue - a.blue) * f);
  return lv_color_make(r, g, bl);
}

// Background colour for the heater tile given the seconds remaining. Outside the
// final window it's the normal brown; inside it eases brown->amber->red.
static lv_color_t heater_bg_for_remaining(int rem_secs) {
  if (rem_secs < 0 || rem_secs >= EXPIRY_FADE_SECS)
    return COL_HEATER;
  // t: 1.0 at the window start (5 min left) -> 0.0 at expiry.
  float t = (float) rem_secs / (float) EXPIRY_FADE_SECS;
  if (t >= 0.5f)
    return lerp_color(COL_HEATER_AMBER, COL_HEATER, (t - 0.5f) / 0.5f);
  return lerp_color(COL_HEATER_RED, COL_HEATER_AMBER, t / 0.5f);
}

#define COL_BTN lv_color_hex(0x2E2E2E)
#define COL_SCREEN_TILE lv_color_hex(0x14424F)
#define COL_SEL lv_color_hex(0xFFD54A)

// event-callback trampolines (run on the LVGL task)
static void ev_heater_roller(lv_event_t *e) {  // scroll picker -> set minutes
  static_cast<PatioUI *>(lv_event_get_user_data(e))->on_heater_roller_changed();
}
static void ev_heater_cancel(lv_event_t *e) {  // Cancel: stop if running, else reset
  static_cast<PatioUI *>(lv_event_get_user_data(e))->on_heater_cancel();
}
static void ev_heater_action(lv_event_t *e) {  // Start (idle) / +15 min (running)
  static_cast<PatioUI *>(lv_event_get_user_data(e))->on_heater_action();
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
static void ev_light_slider(lv_event_t *e) {  // dim fader released -> push brightness
  auto *c = static_cast<PatioUI::LightCtrl *>(lv_event_get_user_data(e));
  int v = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(e)));
  c->self->request_light_brightness(c->idx, v);
}
static void ev_light_toggle(lv_event_t *e) {  // tap group name -> toggle on/off
  auto *c = static_cast<PatioUI::LightCtrl *>(lv_event_get_user_data(e));
  c->self->request_light_toggle(c->idx);
}
static void ev_light_dragging(lv_event_t *e) {  // fader value changing -> resize peach fill
  auto *c = static_cast<PatioUI::LightCtrl *>(lv_event_get_user_data(e));
  int v = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(e)));
  c->self->update_light_fill_(c->idx, v, v > 0);
}
static void ev_media_playpause(lv_event_t *e) {  // deck play/pause toggle
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_media_cmd(1);
}
static void ev_media_next(lv_event_t *e) {  // deck next track
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_media_cmd(2);
}
static void ev_media_prev(lv_event_t *e) {  // deck previous track
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_media_cmd(3);
}
static void ev_media_vol(lv_event_t *e) {  // volume fader released -> push level
  static_cast<PatioUI *>(lv_event_get_user_data(e))
      ->request_media_volume(lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(e))));
}
static void ev_media_vol_live(lv_event_t *e) {  // fader dragging -> update the % label live
  auto *self = static_cast<PatioUI *>(lv_event_get_user_data(e));
  int v = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(e)));
  self->update_media_vol_label_(v);
}

static lv_obj_t *make_tile_title(lv_obj_t *parent, const char *txt) {
  lv_obj_t *t = lv_label_create(parent);
  lv_label_set_text(t, txt);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_opa(t, LV_OPA_70, 0);  // quiet header, not competing with content
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
  return t;
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

void PatioUI::build_ui_() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  lv_obj_t *tv = lv_tileview_create(scr);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);  // replaced by page dots below
  this->tv_ = tv;

  lv_obj_t *t_time = lv_tileview_add_tile(tv, 0, 0, LV_DIR_HOR);
  lv_obj_t *t_heater = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
  lv_obj_t *t_lights = lv_tileview_add_tile(tv, 2, 0, LV_DIR_HOR);
  lv_obj_t *t_screens = lv_tileview_add_tile(tv, 3, 0, LV_DIR_HOR);
  lv_obj_t *t_media = lv_tileview_add_tile(tv, 4, 0, LV_DIR_HOR);
  this->tiles_[TILE_TIME] = t_time;
  this->tiles_[TILE_HEATER] = t_heater;
  this->tiles_[TILE_LIGHTS] = t_lights;
  this->tiles_[TILE_SCREENS] = t_screens;
  this->tiles_[TILE_MEDIA] = t_media;

  // --- clock + outside-temperature tile (the resting/idle screen) ---
  this->build_time_tile_(t_time);

  // --- heater tile (live, wired to HA): iOS-timer style picker ---
  //   idle    : scroll the roller to pick 15/30/45/60 min; Start begins the run
  //   running : big MM:SS countdown; Cancel stops, "+15 min" extends the run
  lv_obj_set_style_bg_color(t_heater, COL_HEATER, 0);
  lv_obj_set_style_bg_opa(t_heater, LV_OPA_COVER, 0);
  make_tile_title(t_heater, "Heater");

  // Vertical scroll picker (idle only). Options map 0..3 -> 15/30/45/60 min.
  lv_obj_t *roller = lv_roller_create(t_heater);
  lv_roller_set_options(roller, "15 min\n30 min\n45 min\n60 min", LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(roller, 3);
  lv_obj_set_width(roller, 180);
  lv_obj_align(roller, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(roller, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(roller, LV_OPA_40, LV_PART_MAIN);      // dim the unselected rows
  lv_obj_set_style_text_font(roller, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_bg_color(roller, lv_color_white(), LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(roller, LV_OPA_10, LV_PART_SELECTED);    // subtle centre band
  lv_obj_set_style_text_color(roller, lv_color_white(), LV_PART_SELECTED);
  lv_obj_set_style_text_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
  lv_obj_set_style_text_font(roller, &lv_font_montserrat_28, LV_PART_SELECTED);
  {
    int sel = this->setpoint_minutes_.load() / 15 - 1;
    if (sel < 0) sel = 0;
    if (sel > 3) sel = 3;
    lv_roller_set_selected(roller, sel, LV_ANIM_OFF);
  }
  lv_obj_add_event_cb(roller, ev_heater_roller, LV_EVENT_VALUE_CHANGED, this);
  this->heater_roller_ = roller;

  // Big MM:SS countdown (running only). Same slot as the roller. Uses the
  // largest built-in font (montserrat 48); a transform-based zoom was tried but
  // it deadlocks lv_snapshot (the /screenshot endpoint), so it's avoided.
  this->heater_value_ = lv_label_create(t_heater);
  lv_obj_set_style_text_color(this->heater_value_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->heater_value_, &patio_font_countdown, 0);
  lv_label_set_text(this->heater_value_, "--");
  lv_obj_align(this->heater_value_, LV_ALIGN_TOP_MID, 0, 60);

  // Bottom action row: End Now (left, running only) + Start / "+15 min" (right).
  this->heater_btn_left_ = make_btn(t_heater, "End Now", ev_heater_cancel, this);
  lv_obj_set_size(this->heater_btn_left_, 132, 48);
  lv_obj_align(this->heater_btn_left_, LV_ALIGN_BOTTOM_LEFT, 14, -22);
  lv_obj_set_style_bg_color(this->heater_btn_left_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->heater_btn_left_, LV_OPA_30, 0);

  this->heater_btn_right_ = make_btn(t_heater, "Start", ev_heater_action, this);
  lv_obj_set_size(this->heater_btn_right_, 132, 48);
  lv_obj_align(this->heater_btn_right_, LV_ALIGN_BOTTOM_RIGHT, -14, -22);
  lv_obj_set_style_bg_color(this->heater_btn_right_, lv_color_hex(0xFFB870), 0);
  lv_obj_set_style_bg_opa(this->heater_btn_right_, LV_OPA_COVER, 0);
  this->heater_btn_right_lbl_ = lv_obj_get_child(this->heater_btn_right_, 0);
  lv_obj_set_style_text_color(this->heater_btn_right_lbl_, lv_color_black(), 0);

  // --- lights tile (live, wired to HA dimmable lights) ---
  this->build_lights_tile_(t_lights);

  // --- screens tile (live perimeter map, wired to HA covers) ---
  this->build_screens_tile_(t_screens);

  // --- deck media tile (live, wired to a HA media_player) ---
  this->build_media_tile_(t_media);

  // Bottom page-position dots (replaces the tileview scroll line). Live on the
  // screen so they float over every tile; updated when the active tile changes.
  lv_obj_t *dots = lv_obj_create(scr);
  lv_obj_remove_style_all(dots);
  lv_obj_set_size(dots, 90, 14);
  lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -3);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots, 9, 0);
  lv_obj_clear_flag(dots, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < NUM_TILES; i++) {
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
  // Fast red/white flash driver for the final EXPIRY_FLASH_SECS (LVGL task).
  this->flash_timer_ = lv_timer_create(PatioUI::flash_cb_, 350, this);
  this->refresh_heater_ui_();
}

// Highlights the dot for the currently-active tile (bright), dims the others.
void PatioUI::update_page_dots_() {
  if (this->tv_ == nullptr)
    return;
  lv_obj_t *active = lv_tileview_get_tile_active(this->tv_);
  for (int i = 0; i < NUM_TILES; i++) {
    if (this->page_dots_[i] == nullptr)
      continue;
    bool on = (this->tiles_[i] == active);
    lv_obj_set_style_bg_opa(this->page_dots_[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
}

// --- clock + outside-temperature tile (the resting screen) ---
void PatioUI::build_time_tile_(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, COL_TIME, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

  // Quiet header: weekday + date (filled in by refresh_time_tile_).
  this->time_date_ = make_tile_title(tile, "");

  // Time + AM/PM share one auto-centred row: the AM/PM hangs to the right of the
  // big digits and the pair stays centred as the clock's width changes.
  lv_obj_t *row = lv_obj_create(tile);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 56);

  // Large 12-hour clock, reusing the 96px digit+colon font.
  this->time_big_ = lv_label_create(row);
  lv_obj_set_style_text_color(this->time_big_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->time_big_, &patio_font_countdown, 0);
  lv_label_set_text(this->time_big_, "");

  // AM/PM indicator, to the right of and baseline-aligned under the clock.
  this->time_ampm_ = lv_label_create(row);
  lv_obj_set_style_text_color(this->time_ampm_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->time_ampm_, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_opa(this->time_ampm_, LV_OPA_80, 0);
  lv_obj_set_style_pad_bottom(this->time_ampm_, 16, 0);  // lift off the digit descenders
  lv_label_set_text(this->time_ampm_, "");

  // Outside temperature (displayed in °C) — larger, lifted up from the bottom.
  this->temp_label_ = lv_label_create(tile);
  lv_obj_set_style_text_color(this->temp_label_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->temp_label_, &lv_font_montserrat_48, 0);
  lv_label_set_text(this->temp_label_, "--\u00B0C");
  lv_obj_align(this->temp_label_, LV_ALIGN_BOTTOM_MID, 0, -40);
}

// Repaints the clock (12-hour) and outside temperature. LVGL task only.
void PatioUI::refresh_time_tile_() {
  if (this->time_big_ != nullptr && this->time_ != nullptr) {
    auto now = this->time_->now();
    if (now.is_valid()) {
      int h12 = now.hour % 12;
      if (h12 == 0)
        h12 = 12;
      char tb[8];
      snprintf(tb, sizeof(tb), "%d:%02d", h12, now.minute);
      lv_label_set_text(this->time_big_, tb);
      lv_label_set_text(this->time_ampm_, now.hour < 12 ? "AM" : "PM");
      char db[32];
      now.strftime(db, sizeof(db), "%a %b %d");
      lv_label_set_text(this->time_date_, db);
    }
  }
  if (this->temp_label_ != nullptr && this->temp_dirty_.exchange(false)) {
    if (this->temp_valid_.load()) {
      float c = this->outside_temp_raw_.load();
      if (this->temp_is_f_.load())
        c = (c - 32.0f) * 5.0f / 9.0f;
      char cb[24];
      snprintf(cb, sizeof(cb), "%.1f\u00B0C", c);
      lv_label_set_text(this->temp_label_, cb);
    } else {
      lv_label_set_text(this->temp_label_, "--\u00B0C");
    }
  }
}

// After IDLE_REVERT_MS with no touch, drift back to the clock tile — unless a
// heater timer is running, in which case rest on the heater/countdown tile so
// the remaining time stays visible. LVGL task only.
void PatioUI::maybe_auto_revert_() {
  if (this->tv_ == nullptr)
    return;
  if (lv_display_get_inactive_time(nullptr) < IDLE_REVERT_MS)
    return;
  int target = this->active_.load() ? TILE_HEATER : TILE_TIME;
  if (this->tiles_[target] == nullptr)
    return;
  if (lv_tileview_get_tile_active(this->tv_) == this->tiles_[target])
    return;
  lv_tileview_set_tile(this->tv_, this->tiles_[target], LV_ANIM_ON);
  this->update_page_dots_();
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
// Scroll picker moved (LVGL task): option idx 0..3 -> 15/30/45/60 min.
void PatioUI::on_heater_roller_changed() {
  if (this->heater_roller_ == nullptr)
    return;
  int sel = lv_roller_get_selected(this->heater_roller_);
  this->setpoint_minutes_.store((sel + 1) * 15);
}
// Left button (LVGL task): stop a running timer, or reset the picker when idle.
void PatioUI::on_heater_cancel() {
  if (this->active_.load()) {
    this->request_stop();
  } else if (this->heater_roller_ != nullptr) {
    int def = this->default_minutes_;
    this->setpoint_minutes_.store(def);
    int sel = def / 15 - 1;
    if (sel < 0)
      sel = 0;
    if (sel > 3)
      sel = 3;
    lv_roller_set_selected(this->heater_roller_, sel, LV_ANIM_ON);
  }
}
// Right button (LVGL task): Start when idle, "+15 min" (extend) when running.
void PatioUI::on_heater_action() {
  if (this->active_.load())
    this->request_extend(15);
  else
    this->request_start();
}
// Extend a running timer by exactly add_min minutes, preserving the current
// seconds. run_script only accepts whole minutes, so we restart the HA timer
// itself with a second-precise H:MM:SS duration (the heater switch is already
// on; the timer's own finish is what turns it back off). Optimistically bump
// the local countdown so the UI reacts immediately.
void PatioUI::request_extend(int add_min) {
  int rem = this->countdown_secs_.load();
  if (rem < 0)
    rem = 0;
  int total = rem + add_min * 60;
  // Never let +15 push the timer past the configured maximum.
  int cap = this->max_minutes_ * 60;
  if (total > cap)
    total = cap;
  if (total <= rem)
    return;  // already at (or above) the cap — nothing to do
  this->pending_extend_secs_.store(total);
  this->countdown_secs_.store(total);
  this->label_dirty_.store(true);
}

// ---------------- LVGL-task label rendering ----------------
void PatioUI::tick_cb_(lv_timer_t *t) { static_cast<PatioUI *>(lv_timer_get_user_data(t))->tick_(); }

// Fast flasher: in the final EXPIRY_FLASH_SECS, alternate the heater tile
// between vivid red and white (with inverted countdown text) for maximum
// "get up and check the heaters" visibility. Outside that window it restores
// the normal look exactly once, then stays out of the way.
void PatioUI::flash_cb_(lv_timer_t *t) { static_cast<PatioUI *>(lv_timer_get_user_data(t))->flash_tick_(); }

void PatioUI::flash_tick_() {
  if (this->heater_value_ == nullptr || this->tiles_[TILE_HEATER] == nullptr)
    return;
  int s = this->countdown_secs_.load();
  bool in_window = this->active_.load() && s > 0 && s <= EXPIRY_FLASH_SECS;
  if (in_window) {
    this->flash_on_ = !this->flash_on_;
    lv_color_t bg = this->flash_on_ ? lv_color_white() : COL_FLASH;
    lv_color_t fg = this->flash_on_ ? COL_FLASH : lv_color_white();
    lv_obj_set_style_bg_color(this->tiles_[TILE_HEATER], bg, 0);
    lv_obj_set_style_text_color(this->heater_value_, fg, 0);
    this->flashing_ = true;
  } else if (this->flashing_) {
    // Leaving the flash window — restore normal text colour and let the next
    // refresh repaint the proper (faded/brown) tile background.
    this->flashing_ = false;
    this->flash_on_ = false;
    lv_obj_set_style_text_color(this->heater_value_, lv_color_white(), 0);
    this->label_dirty_.store(true);
  }
}

void PatioUI::tick_() {
  // Countdown while active. Prefer the absolute finish time (authoritative and
  // self-correcting across reboots); fall back to a local 1 Hz decrement until
  // the finish time and a valid clock are both available.
  if (this->active_.load()) {
    long fin = this->finishes_at_epoch_.load();
    bool derived = false;
    if (fin > 0 && this->time_ != nullptr) {
      auto now = this->time_->now();
      if (now.is_valid()) {
        long rem = fin - static_cast<long>(now.timestamp);
        if (rem < 0)
          rem = 0;
        this->countdown_secs_.store(static_cast<int>(rem));
        derived = true;
      }
    }
    if (!derived) {
      int s = this->countdown_secs_.load();
      if (s > 0)
        this->countdown_secs_.store(s - 1);
    }
    this->label_dirty_.store(true);
  }
  if (this->label_dirty_.exchange(false))
    this->refresh_heater_ui_();
  if (this->light_ui_dirty_.exchange(false))
    this->refresh_lights_ui_();
  if (this->media_ui_dirty_.exchange(false))
    this->refresh_media_ui_();
  // Clock tile refresh + idle auto-revert (both LVGL-task safe).
  this->refresh_time_tile_();
  this->maybe_auto_revert_();
}

// Redraw the timer tile for the current state (LVGL task).
//   idle    : show the scroll picker; right button = "Start".
//   running : show the big MM:SS countdown; left = "Cancel", right = "+15 min".
void PatioUI::refresh_heater_ui_() {
  if (this->heater_value_ == nullptr)
    return;
  bool active = this->active_.load();

  if (active) {
    int s = this->countdown_secs_.load();
    if (s < 0)
      s = 0;
    char buf[16];
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0)
      snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else
      snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
    lv_label_set_text(this->heater_value_, buf);
  }

  // Nearing-expiry indicator: fade the tile background brown->amber->red over
  // the final EXPIRY_FADE_SECS. Normal brown while idle or with time to spare.
  // While the final-seconds flasher owns the tile, leave the background alone.
  if (this->tiles_[TILE_HEATER] != nullptr && !this->flashing_) {
    lv_color_t bg = active ? heater_bg_for_remaining(this->countdown_secs_.load()) : COL_HEATER;
    lv_obj_set_style_bg_color(this->tiles_[TILE_HEATER], bg, 0);
  }

  // Toggle picker vs countdown.
  if (this->heater_roller_ != nullptr) {
    if (active) {
      lv_obj_add_flag(this->heater_roller_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(this->heater_roller_, LV_OBJ_FLAG_HIDDEN);
      // Reflect the current setpoint (which resets to the configured default
      // when a timer ends) so the picker doesn't linger on a stale value.
      int sel = this->setpoint_minutes_.load() / 15 - 1;
      if (sel < 0)
        sel = 0;
      if (sel > 3)
        sel = 3;
      lv_roller_set_selected(this->heater_roller_, sel, LV_ANIM_OFF);
    }
  }
  if (active)
    lv_obj_remove_flag(this->heater_value_, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(this->heater_value_, LV_OBJ_FLAG_HIDDEN);

  // Right button label: Start / +15 min. Left (End Now) shows only while running.
  if (this->heater_btn_right_lbl_ != nullptr)
    lv_label_set_text(this->heater_btn_right_lbl_, active ? "+15 min" : "Start");
  if (this->heater_btn_left_ != nullptr) {
    if (active)
      lv_obj_remove_flag(this->heater_btn_left_, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(this->heater_btn_left_, LV_OBJ_FLAG_HIDDEN);
  }
  if (this->heater_btn_right_ != nullptr) {
    if (active)
      lv_obj_align(this->heater_btn_right_, LV_ALIGN_BOTTOM_RIGHT, -14, -22);
    else
      lv_obj_align(this->heater_btn_right_, LV_ALIGN_BOTTOM_MID, 0, -22);  // Start centered
  }
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
  // vertically centered against the left/right side screens (their center ~y82).
  this->screen_btn_[2] = make_screen_button(tile, &this->screen_tap_[2], SCR_REAR);
  lv_obj_set_size(this->screen_btn_[2], 88, 52);
  lv_obj_align(this->screen_btn_[2], LV_ALIGN_TOP_MID, -47, 56);

  this->screen_btn_[3] = make_screen_button(tile, &this->screen_tap_[3], SCR_REAR);
  lv_obj_set_size(this->screen_btn_[3], 88, 52);
  lv_obj_align(this->screen_btn_[3], LV_ALIGN_TOP_MID, 47, 56);

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

// ---------------- lights tile (LVGL task) ----------------
// Two vertical dim faders (Main | BBQ). Tapping a group name toggles it on/off;
// dragging its fader sets brightness (0 => off). Both directions are live: HA
// state changes are reflected back onto the faders via refresh_lights_ui_().
void PatioUI::build_lights_tile_(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, COL_LIGHTS, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  make_tile_title(tile, "Lights");

  const int col_dx[NUM_LIGHTS] = {-80, 80};  // two columns centred on the tile
  for (int i = 0; i < NUM_LIGHTS; i++) {
    this->light_ctrl_[i] = LightCtrl{this, i};
    bool cfg = this->light_configured_[i];

    // group name — tap target that toggles the light on/off. Kept at the very
    // top so the fully-raised (on) knob never reaches up into it: that lets us
    // give the knob a large grab area without stealing taps meant for the label.
    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, cfg ? this->light_label_[i].c_str() : (i == 0 ? "Main" : "BBQ"));
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, col_dx[i], 44);
    this->light_name_[i] = name;
    if (cfg) {
      lv_obj_add_flag(name, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(name, 16);  // enlarge the touch target
      lv_obj_add_event_cb(name, ev_light_toggle, LV_EVENT_CLICKED, &this->light_ctrl_[i]);
    }

    // vertical dim fader, built as three stacked layers so the peach fill can
    // reach the track bottom while the knob stays clamped inside the track:
    //   1. `track`  — constant translucent recess (the light base strip).
    //   2. `fill`   — peach layer we size by hand, always anchored to the track
    //                 bottom and growing up to the knob. Because we draw it
    //                 ourselves it isn't subject to the slider's LV_PART_MAIN
    //                 padding inset (which used to leave a gap at the bottom).
    //   3. `sl`     — the slider itself, with a transparent track and indicator
    //                 so only the white knob shows. Knob-only interaction
    //                 (ADV_HITTEST); LV_PART_MAIN top/bottom padding == half the
    //                 knob height clamps the knob travel inside the track, so it
    //                 never dips into the page-dot / bottom-edge swipe zone.
    // Layers are created in back-to-front order (track, fill, sl) so the knob
    // draws over the fill and the fill over the track.
    lv_obj_t *track = lv_obj_create(tile);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(track, 46, 110);
    lv_obj_align(track, LV_ALIGN_TOP_MID, col_dx[i], 82);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);  // subtle recess, not a black blob
    lv_obj_set_style_radius(track, 8, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);

    lv_obj_t *fill = lv_obj_create(tile);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(fill, LV_OBJ_FLAG_FLOATING);  // we set its size/pos by hand
    lv_obj_set_width(fill, 46);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0xF2C879), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, 8, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_pad_all(fill, 0, 0);
    this->light_fill_[i] = fill;

    lv_obj_t *sl = lv_slider_create(tile);
    lv_obj_set_size(sl, 46, 110);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, col_dx[i], 82);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_MAIN);       // track drawn by `track`
    lv_obj_set_style_pad_top(sl, 15, LV_PART_MAIN);     // == knob half-height: clamp travel
    lv_obj_set_style_pad_bottom(sl, 15, LV_PART_MAIN);  // so knob stays inside the track
    lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_INDICATOR);  // fill drawn by `fill`
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);  // knob
    // Thin wide pill knob: 46 wide x 30 tall (base 46, negative vertical pad).
    lv_obj_set_style_pad_top(sl, -8, LV_PART_KNOB);
    lv_obj_set_style_pad_bottom(sl, -8, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 10, LV_PART_KNOB);
    lv_obj_add_flag(sl, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(sl, 10);  // small forgiveness halo around the knob
    this->light_slider_[i] = sl;
    if (cfg) {
      lv_obj_add_event_cb(sl, ev_light_slider, LV_EVENT_RELEASED, &this->light_ctrl_[i]);
      lv_obj_add_event_cb(sl, ev_light_dragging, LV_EVENT_VALUE_CHANGED, &this->light_ctrl_[i]);
    } else {
      lv_obj_add_state(sl, LV_STATE_DISABLED);
      lv_obj_set_style_opa(sl, LV_OPA_40, 0);
      lv_obj_set_style_opa(name, LV_OPA_40, 0);
    }
    this->update_light_fill_(i, 0, false);
  }
}

// Size/position the peach fill layer: it hugs the track bottom and grows up to
// the knob centre. Track: TOP_MID at y=82, height 110, 15 px MAIN padding =>
// knob centre travels y=177 (value 0) .. y=97 (value 100). Hidden when off.
void PatioUI::update_light_fill_(int i, int value, bool on) {
  lv_obj_t *fill = this->light_fill_[i];
  if (fill == nullptr)
    return;
  if (!on || value <= 0) {
    lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
  const int col_dx[NUM_LIGHTS] = {-80, 80};
  const int track_top = 82, track_h = 110, main_pad = 15;
  const int travel = track_h - 2 * main_pad;                              // 80
  int knob_cy = (track_top + track_h - main_pad) - (value * travel) / 100;  // 165 .. 85
  int fill_bottom = track_top + track_h;                                    // 180
  lv_obj_set_height(fill, fill_bottom - knob_cy);                           // reaches the bottom
  lv_obj_align(fill, LV_ALIGN_TOP_MID, col_dx[i], knob_cy);
}

// Reflect the last-known HA on/off + brightness onto the faders (LVGL task).
void PatioUI::refresh_lights_ui_() {
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (!this->light_configured_[i])
      continue;
    lv_obj_t *sl = this->light_slider_[i];
    lv_obj_t *name = this->light_name_[i];
    bool on = this->light_on_[i].load();
    int pct = on ? this->light_bright_[i].load() : 0;
    // Don't fight the user while they're dragging this fader.
    if (sl != nullptr && !lv_obj_has_state(sl, LV_STATE_PRESSED)) {
      lv_slider_set_value(sl, pct, LV_ANIM_OFF);
      this->update_light_fill_(i, pct, on);
    }
    if (name != nullptr)
      lv_obj_set_style_opa(name, on ? LV_OPA_COVER : LV_OPA_50, 0);
  }
}

// ---------------- deck media tile (LVGL task) ----------------
// Now-playing text, a horizontal volume fader, and a prev / play-pause / next
// control bar wired to a single HA media_player (the deck Sonos amp). Both
// directions are live: HA state changes flow back via refresh_media_ui_().
void PatioUI::build_media_tile_(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, COL_MEDIA, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  make_tile_title(tile, this->media_label_.c_str());

  // Now-playing line: track title if HA gives us one, else the state word.
  this->media_title_lbl_ = lv_label_create(tile);
  lv_obj_set_style_text_color(this->media_title_lbl_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->media_title_lbl_, &lv_font_montserrat_20, 0);
  lv_label_set_long_mode(this->media_title_lbl_, LV_LABEL_LONG_DOT);
  lv_obj_set_width(this->media_title_lbl_, 280);
  lv_obj_set_style_text_align(this->media_title_lbl_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(this->media_title_lbl_, "");
  lv_obj_align(this->media_title_lbl_, LV_ALIGN_TOP_MID, 0, 46);

  // Volume row: speaker icon + horizontal fader + "NN%".
  lv_obj_t *vicon = lv_label_create(tile);
  lv_obj_set_style_text_color(vicon, lv_color_white(), 0);
  lv_obj_set_style_text_font(vicon, &lv_font_montserrat_20, 0);
  lv_label_set_text(vicon, LV_SYMBOL_VOLUME_MID);
  lv_obj_align(vicon, LV_ALIGN_TOP_LEFT, 14, 104);

  this->media_vol_slider_ = lv_slider_create(tile);
  lv_obj_set_size(this->media_vol_slider_, 210, 12);
  lv_obj_align(this->media_vol_slider_, LV_ALIGN_TOP_MID, 6, 108);
  lv_slider_set_range(this->media_vol_slider_, 0, 100);
  lv_slider_set_value(this->media_vol_slider_, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(this->media_vol_slider_, lv_color_hex(0x2A1C3E), LV_PART_MAIN);
  lv_obj_set_style_bg_color(this->media_vol_slider_, lv_color_hex(0xB98CE6), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(this->media_vol_slider_, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_ext_click_area(this->media_vol_slider_, 12);
  lv_obj_add_event_cb(this->media_vol_slider_, ev_media_vol, LV_EVENT_RELEASED, this);
  lv_obj_add_event_cb(this->media_vol_slider_, ev_media_vol_live, LV_EVENT_VALUE_CHANGED, this);

  this->media_vol_pct_ = lv_label_create(tile);
  lv_obj_set_style_text_color(this->media_vol_pct_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->media_vol_pct_, &lv_font_montserrat_20, 0);
  lv_label_set_text(this->media_vol_pct_, "--%");
  lv_obj_align(this->media_vol_pct_, LV_ALIGN_TOP_RIGHT, -14, 104);

  // Transport bar: prev / play-pause / next.
  lv_obj_t *bar = lv_obj_create(tile);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 320, 50);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *b_prev = make_btn(bar, LV_SYMBOL_PREV, ev_media_prev, this);
  lv_obj_set_size(b_prev, 98, 44);
  lv_obj_t *b_pp = make_btn(bar, LV_SYMBOL_PLAY, ev_media_playpause, this);
  lv_obj_set_size(b_pp, 98, 44);
  this->media_playpause_lbl_ = lv_obj_get_child(b_pp, 0);  // the symbol label
  lv_obj_t *b_next = make_btn(bar, LV_SYMBOL_NEXT, ev_media_next, this);
  lv_obj_set_size(b_next, 98, 44);

  // No entity bound -> show a disabled placeholder rather than dead controls.
  if (!this->media_configured_) {
    lv_label_set_text(this->media_title_lbl_, "(no media player)");
    lv_obj_t *ctrls[3] = {b_prev, b_pp, b_next};
    for (auto *c : ctrls) {
      lv_obj_add_state(c, LV_STATE_DISABLED);
      lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_opa(c, LV_OPA_40, 0);
    }
    lv_obj_add_state(this->media_vol_slider_, LV_STATE_DISABLED);
    lv_obj_set_style_opa(this->media_vol_slider_, LV_OPA_40, 0);
  }
}

// Live "NN%" while the volume fader is being dragged (LVGL task).
void PatioUI::update_media_vol_label_(int pct) {
  if (this->media_vol_pct_ == nullptr)
    return;
  char b[8];
  snprintf(b, sizeof(b), "%d%%", pct);
  lv_label_set_text(this->media_vol_pct_, b);
}

// Reflect last-known HA state (now-playing, volume, play/pause icon) onto the
// media tile (LVGL task).
void PatioUI::refresh_media_ui_() {
  if (!this->media_configured_)
    return;
  int st = this->media_state_.load();

  // Now-playing: prefer the pushed title, fall back to a state word.
  char title[64];
  portENTER_CRITICAL(&this->media_title_mux_);
  strncpy(title, this->media_title_, sizeof(title) - 1);
  title[sizeof(title) - 1] = '\0';
  portEXIT_CRITICAL(&this->media_title_mux_);
  if (this->media_title_lbl_ != nullptr) {
    if (title[0] != '\0')
      lv_label_set_text(this->media_title_lbl_, title);
    else
      lv_label_set_text(this->media_title_lbl_,
                        st == 1 ? "Playing" : st == 2 ? "Paused" : "Stopped");
  }

  // Volume — skip while the user is dragging the fader.
  int vol = this->media_vol_.load();
  if (this->media_vol_slider_ != nullptr && vol >= 0 &&
      !lv_obj_has_state(this->media_vol_slider_, LV_STATE_PRESSED)) {
    lv_slider_set_value(this->media_vol_slider_, vol, LV_ANIM_OFF);
    this->update_media_vol_label_(vol);
  }

  // Play/pause button shows the action it will perform.
  if (this->media_playpause_lbl_ != nullptr)
    lv_label_set_text(this->media_playpause_lbl_, st == 1 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// ---------------- media intents (LVGL task) ----------------
void PatioUI::request_media_cmd(int cmd) {
  if (!this->media_configured_)
    return;
  this->pending_media_cmd_.store(cmd);
  // Optimistically flip the play/pause icon so the button reacts instantly
  // (called on the LVGL task, so touching the label is safe); HA's later state
  // push reconciles it if the command was rejected.
  if (cmd == 1) {
    int nst = (this->media_state_.load() == 1) ? 2 : 1;  // playing <-> paused
    this->media_state_.store(nst);
    if (this->media_playpause_lbl_ != nullptr)
      lv_label_set_text(this->media_playpause_lbl_, nst == 1 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  }
}
void PatioUI::request_media_volume(int pct) {
  if (!this->media_configured_)
    return;
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  this->pending_media_vol_.store(pct);
}

// ---------------- media HA -> UI callbacks (main/API task) ----------------
void PatioUI::on_media_state_(std::string state) {
  int st = (state == "playing") ? 1 : (state == "paused") ? 2 : 0;
  this->media_state_.store(st);
  this->media_ui_dirty_.store(true);
  ESP_LOGD(TAG, "media state -> %s (%d)", state.c_str(), st);
}
void PatioUI::on_media_volume_(std::string volume) {
  // HA 'volume_level' is a 0.0..1.0 float -> 0..100 %.
  char *end = nullptr;
  float v = strtof(volume.c_str(), &end);
  if (end != volume.c_str()) {
    int pct = static_cast<int>(v * 100.0f + 0.5f);
    if (pct < 0)
      pct = 0;
    if (pct > 100)
      pct = 100;
    this->media_vol_.store(pct);
    this->media_ui_dirty_.store(true);
    ESP_LOGD(TAG, "media volume -> %s (%d%%)", volume.c_str(), pct);
  }
}
void PatioUI::on_media_title_(std::string title) {
  if (title == "unknown" || title == "unavailable" || title == "None")
    title.clear();
  portENTER_CRITICAL(&this->media_title_mux_);
  strncpy(this->media_title_, title.c_str(), sizeof(this->media_title_) - 1);
  this->media_title_[sizeof(this->media_title_) - 1] = '\0';
  portEXIT_CRITICAL(&this->media_title_mux_);
  this->media_ui_dirty_.store(true);
  ESP_LOGD(TAG, "media title -> %s", title.c_str());
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
void PatioUI::persist_finishes_at_(long epoch) {
  this->finishes_pref_.save(&epoch);
  global_preferences->sync();
}

void PatioUI::on_timer_state_(std::string state) {
  bool active = (state == "active");
  this->active_.store(active);
  if (!active) {
    this->countdown_secs_.store(-1);
    this->finishes_at_epoch_.store(0);
    this->persist_finishes_at_(0);
    // Return the picker to the configured default for the next run.
    this->setpoint_minutes_.store(this->default_minutes_);
  }
  this->label_dirty_.store(true);
  ESP_LOGD(TAG, "timer state: %s", state.c_str());
}

void PatioUI::on_timer_remaining_(std::string remaining) {
  // "H:MM:SS" -> seconds. NOTE: HA freezes this value while the timer runs, so
  // it only seeds an approximate countdown until on_timer_finishes_at_ + a valid
  // clock take over (see tick_()).
  int h = 0, m = 0, s = 0;
  if (sscanf(remaining.c_str(), "%d:%d:%d", &h, &m, &s) == 3) {
    this->countdown_secs_.store(h * 3600 + m * 60 + s);
    this->label_dirty_.store(true);
    ESP_LOGD(TAG, "timer remaining: %s (%d s)", remaining.c_str(), h * 3600 + m * 60 + s);
  }
}

void PatioUI::on_timer_finishes_at_(std::string finishes_at) {
  // Absolute UTC ISO-8601, e.g. "2026-08-10T17:45:00+00:00". HA always sends
  // this in UTC, so parse the wall-clock fields and convert with timegm().
  if (finishes_at.empty() || finishes_at == "None" || finishes_at == "unknown") {
    this->finishes_at_epoch_.store(0);
    this->persist_finishes_at_(0);
    return;
  }
  int yr = 0, mo = 0, dy = 0, hr = 0, mi = 0, se = 0;
  if (sscanf(finishes_at.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mi, &se) == 6) {
    // Days since the Unix epoch for this UTC calendar date (Howard Hinnant's
    // civil-from-days algorithm). Avoids timegm(), which ESP-IDF's newlib lacks.
    int y = yr - (mo <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + dy - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
    long epoch = days * 86400L + hr * 3600L + mi * 60L + se;
    this->finishes_at_epoch_.store(epoch);
    this->persist_finishes_at_(epoch);
    this->label_dirty_.store(true);
    ESP_LOGD(TAG, "timer finishes_at: %s (%ld)", finishes_at.c_str(), epoch);
  }
}

// ---------------- lights (bidirectional: HA <-> panel) ----------------
void PatioUI::add_light(int slot, const std::string &entity, const std::string &label) {
  if (slot < 0 || slot >= NUM_LIGHTS)
    return;
  this->light_entity_[slot] = entity;
  this->light_label_[slot] = label;
  this->light_configured_[slot] = true;
}

int PatioUI::light_index_for_entity_(const std::string &entity_id) const {
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (this->light_configured_[i] && this->light_entity_[i] == entity_id)
      return i;
  }
  return -1;
}

// UI -> HA intents (called from the LVGL task; only touch atomics).
void PatioUI::request_light_brightness(int idx, int pct) {
  if (idx < 0 || idx >= NUM_LIGHTS || !this->light_configured_[idx])
    return;
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  this->pending_light_bright_[idx].store(pct);
}
void PatioUI::request_light_toggle(int idx) {
  if (idx < 0 || idx >= NUM_LIGHTS || !this->light_configured_[idx])
    return;
  this->pending_light_toggle_[idx].store(true);
}

// HA -> UI state callbacks (run on the main/API task; only touch atomics).
void PatioUI::on_light_state_(std::string entity_id, std::string state) {
  int idx = this->light_index_for_entity_(entity_id);
  if (idx < 0)
    return;
  this->light_on_[idx].store(state == "on");
  this->light_ui_dirty_.store(true);
  ESP_LOGD(TAG, "light[%d] %s -> %s", idx, entity_id.c_str(), state.c_str());
}
void PatioUI::on_light_bright_(std::string entity_id, std::string brightness) {
  int idx = this->light_index_for_entity_(entity_id);
  if (idx < 0)
    return;
  // HA 'brightness' attribute is 0..255 (empty/"None" when off) -> 0..100 %.
  int b255 = atoi(brightness.c_str());
  int pct = (b255 <= 0) ? 0 : (b255 * 100 + 127) / 255;
  if (pct > 0)  // keep last non-zero level so an off->on toggle can restore it
    this->light_bright_[idx].store(pct);
  this->light_ui_dirty_.store(true);
  ESP_LOGD(TAG, "light[%d] brightness %d/255 (%d%%)", idx, b255, pct);
}

// Outside temperature for the clock tile. Stored in the sensor's native unit;
// converted to °C at display time (see refresh_time_tile_). Empty / "unknown" /
// "unavailable" mark it invalid so the label shows a placeholder.
void PatioUI::on_outside_temp_(std::string state) {
  if (state.empty() || state == "unknown" || state == "unavailable" || state == "None") {
    this->temp_valid_.store(false);
    this->temp_dirty_.store(true);
    ESP_LOGD(TAG, "outside temp: %s (invalid)", state.c_str());
    return;
  }
  char *end = nullptr;
  float v = strtof(state.c_str(), &end);
  if (end == state.c_str()) {  // not a number
    this->temp_valid_.store(false);
  } else {
    this->outside_temp_raw_.store(v);
    this->temp_valid_.store(true);
  }
  this->temp_dirty_.store(true);
  ESP_LOGD(TAG, "outside temp: %s", state.c_str());
}

// Native unit of the temperature sensor. If it's Fahrenheit we convert to °C
// for display; anything else is assumed already Celsius.
void PatioUI::on_temp_unit_(std::string unit) {
  bool is_f = (unit.find('F') != std::string::npos);
  this->temp_is_f_.store(is_f);
  this->temp_dirty_.store(true);
  ESP_LOGD(TAG, "outside temp unit: %s (%s)", unit.c_str(), is_f ? "F->C" : "C");
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

  // Silence the AXP2101 charge-indicator LED — the blue light that blinks at
  // 1 Hz on the side of the case. The BSP programs CHGLED (reg 0x69) to the
  // "1 Hz blink" mode (0b00010011); overwrite it with 0x00 (manual, output
  // off) over the BSP's shared I2C bus so the indicator stays dark. This only
  // touches the LED control register, not charging behaviour.
  {
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus != nullptr) {
      i2c_device_config_t axp_cfg = {};
      axp_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
      axp_cfg.device_address = 0x34;  // AXP2101
      axp_cfg.scl_speed_hz = 100000;
      i2c_master_dev_handle_t axp = nullptr;
      if (i2c_master_bus_add_device(i2c_bus, &axp_cfg, &axp) == ESP_OK) {
        const uint8_t chgled_off[] = {0x69, 0x00};
        if (i2c_master_transmit(axp, chgled_off, sizeof(chgled_off), 1000) != ESP_OK)
          ESP_LOGW(TAG, "failed to disable CHGLED");
        i2c_master_bus_rm_device(axp);
      }
    }
  }

  bsp_display_lock(0);
  this->build_ui_();
  bsp_display_unlock();

  // Restore the persisted finish time BEFORE subscribing to HA. On a warm reboot
  // the ESP32 RTC keeps real wall-clock time, so we can show the running
  // countdown immediately rather than waiting for HA to reconnect (~30 s). HA's
  // later state push confirms or corrects this (e.g. clears it if cancelled).
  this->finishes_pref_ = global_preferences->make_preference<long>(fnv1_hash("patio_ui_finishes_at"));
  long saved_fin = 0;
  if (this->finishes_pref_.load(&saved_fin) && saved_fin > 0) {
    bool still_running = true;
    if (this->time_ != nullptr && this->time_->now().is_valid())
      still_running = saved_fin > static_cast<long>(this->time_->now().timestamp);
    if (still_running) {
      this->finishes_at_epoch_.store(saved_fin);
      this->active_.store(true);
      if (this->time_ != nullptr && this->time_->now().is_valid()) {
        long rem = saved_fin - static_cast<long>(this->time_->now().timestamp);
        this->countdown_secs_.store(rem > 0 ? static_cast<int>(rem) : 0);
      }
      this->label_dirty_.store(true);
      ESP_LOGI(TAG, "restored persisted timer finish %ld", saved_fin);
    } else {
      this->persist_finishes_at_(0);  // expired while powered off
    }
  }

  // If a timer is already running at boot, rest on the heater tile so the
  // countdown is visible immediately (otherwise start on the clock tile).
  if (this->active_.load() && this->tv_ != nullptr) {
    bsp_display_lock(0);
    lv_tileview_set_tile(this->tv_, this->tiles_[TILE_HEATER], LV_ANIM_OFF);
    this->update_page_dots_();
    bsp_display_unlock();
  }

  // Subscribe to the HA timer (api is a hard dependency, so global_api_server is up).
  this->subscribe_homeassistant_state(&PatioUI::on_timer_state_, this->timer_entity_);
  this->subscribe_homeassistant_state(&PatioUI::on_timer_remaining_, this->timer_entity_, "remaining");
  this->subscribe_homeassistant_state(&PatioUI::on_timer_finishes_at_, this->timer_entity_, "finishes_at");

  // Clock-tile outside temperature (converted to °C for display; see on_temp_unit_).
  this->subscribe_homeassistant_state(&PatioUI::on_outside_temp_, this->temp_sensor_);
  this->subscribe_homeassistant_state(&PatioUI::on_temp_unit_, this->temp_sensor_, "unit_of_measurement");

  // Screens are Somfy RTS (command-only, no reliable state feedback), so we
  // don't subscribe to their state — the tiles are selectors + momentary
  // up/stop/down commands.

  // Lights are bidirectional: init per-light atomics, then subscribe to HA so
  // the faders reflect external changes (and toggles can restore last level).
  for (int i = 0; i < NUM_LIGHTS; i++) {
    this->pending_light_bright_[i].store(-1);
    this->pending_light_toggle_[i].store(false);
    this->light_bright_[i].store(0);
    this->light_on_[i].store(false);
    if (this->light_configured_[i]) {
      this->subscribe_homeassistant_state(&PatioUI::on_light_state_, this->light_entity_[i]);
      this->subscribe_homeassistant_state(&PatioUI::on_light_bright_, this->light_entity_[i], "brightness");
    }
  }
  this->light_ui_dirty_.store(true);

  // Deck media player: subscribe to state + volume + now-playing title so the
  // media tile reflects external Sonos changes; init the pending intents.
  this->pending_media_vol_.store(-1);
  this->pending_media_cmd_.store(0);
  if (this->media_configured_) {
    this->subscribe_homeassistant_state(&PatioUI::on_media_state_, this->media_entity_);
    this->subscribe_homeassistant_state(&PatioUI::on_media_volume_, this->media_entity_, "volume_level");
    this->subscribe_homeassistant_state(&PatioUI::on_media_title_, this->media_entity_, "media_title");
  }
  this->media_ui_dirty_.store(true);

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
  // Drain a pending extend: restart the HA timer with a second-precise duration
  // (heater is already on; only the timer needs lengthening).
  int ext = this->pending_extend_secs_.exchange(-1);
  if (ext >= 0) {
    int h = ext / 3600, m = (ext % 3600) / 60, s = ext % 60;
    char dur[16];
    snprintf(dur, sizeof(dur), "%d:%02d:%02d", h, m, s);
    ESP_LOGI(TAG, "heater extend -> timer.start %s (%s)", this->timer_entity_.c_str(), dur);
    this->call_homeassistant_service("timer.start",
                                     {{"entity_id", this->timer_entity_}, {"duration", dur}});
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

  // Drain pending light intents against each configured light.
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (!this->light_configured_[i])
      continue;
    int pct = this->pending_light_bright_[i].exchange(-1);
    if (pct >= 0) {
      if (pct == 0) {
        ESP_LOGI(TAG, "light[%d] off -> %s", i, this->light_entity_[i].c_str());
        this->call_homeassistant_service("light.turn_off",
                                         {{"entity_id", this->light_entity_[i]}, {"transition", "0"}});
      } else {
        ESP_LOGI(TAG, "light[%d] dim %d%% -> %s", i, pct, this->light_entity_[i].c_str());
        this->call_homeassistant_service(
            "light.turn_on",
            {{"entity_id", this->light_entity_[i]}, {"brightness_pct", std::to_string(pct)}, {"transition", "0"}});
      }
    }
    if (this->pending_light_toggle_[i].exchange(false)) {
      bool on = this->light_on_[i].load();
      const char *svc2 = on ? "light.turn_off" : "light.turn_on";
      ESP_LOGI(TAG, "light[%d] toggle (%s) -> %s", i, on ? "off" : "on", this->light_entity_[i].c_str());
      // transition:0 so turn-off snaps instead of using the bulb's default
      // fade-out (which makes off feel much slower than on).
      this->call_homeassistant_service(svc2, {{"entity_id", this->light_entity_[i]}, {"transition", "0"}});
    }
  }

  // Drain pending deck-media intents against the Sonos amp.
  if (this->media_configured_) {
    int mv = this->pending_media_vol_.exchange(-1);
    if (mv >= 0) {
      char lvl[8];
      snprintf(lvl, sizeof(lvl), "%.2f", mv / 100.0f);
      ESP_LOGI(TAG, "media volume %d%% -> %s", mv, this->media_entity_.c_str());
      this->call_homeassistant_service("media_player.volume_set",
                                       {{"entity_id", this->media_entity_}, {"volume_level", lvl}});
    }
    int mc = this->pending_media_cmd_.exchange(0);
    if (mc != 0) {
      const char *svc3 = (mc == 1) ? "media_player.media_play_pause"
                         : (mc == 2) ? "media_player.media_next_track"
                                     : "media_player.media_previous_track";
      ESP_LOGI(TAG, "media cmd %d -> %s (%s)", mc, svc3, this->media_entity_.c_str());
      this->call_homeassistant_service(svc3, {{"entity_id", this->media_entity_}});
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
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (this->light_configured_[i])
      ESP_LOGCONFIG(TAG, "  light[%d]  %-9s -> %s", i, this->light_label_[i].c_str(),
                    this->light_entity_[i].c_str());
  }
  if (this->media_configured_)
    ESP_LOGCONFIG(TAG, "  media     %-9s -> %s", this->media_label_.c_str(), this->media_entity_.c_str());
}

}  // namespace patio_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
