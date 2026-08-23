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
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "png_uncompressed.h"

#include "esphome/core/application.h"
#include "esphome/components/network/util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <sys/time.h>

namespace esphome {
namespace patio_ui {

static const char *const TAG = "patio_ui";

// 96px digit+colon font (0-9 and ':') generated with lv_font_conv; compiled as
// C in patio_font_countdown.c, so it needs C linkage here.
extern "C" const lv_font_t patio_font_countdown;

// Single-glyph MDI "wifi-off" font (U+F05AA) for the not-connected icon, also
// generated with lv_font_conv (patio_font_wifi_off.c). UTF-8 for U+F05AA is the
// 4-byte sequence below.
extern "C" const lv_font_t patio_font_wifi_off;
#define PATIO_WIFI_OFF_SYMBOL "\xF3\xB0\x96\xAA"

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

// --- climate (thermostat) tile ---
// Tile background per hvac mode: neutral slate (off), cool blue, warm orange,
// teal (auto). Picked in refresh_climate_ui_ from the current mode.
#define COL_CLIMATE_OFF lv_color_hex(0x37414F)
#define COL_CLIMATE_COOL lv_color_hex(0x1C567E)
#define COL_CLIMATE_HEAT lv_color_hex(0x9A4A16)
#define COL_CLIMATE_AUTO lv_color_hex(0x1E6E5A)

// Full hvac mode table. Index is stored in climate_mode_ atomic; HA names map to
// these indices in on_climate_state_.
enum {
  CLM_OFF = 0,
  CLM_HEAT = 1,
  CLM_COOL = 2,
  CLM_DRY = 3,
  CLM_FAN = 4,
  CLM_AUTO = 5,
  CLM_COUNT = 6,
};
static const char *const CLIMATE_MODE_HA[CLM_COUNT] = {"off", "heat", "cool", "dry", "fan_only", "auto"};
static const char *const CLIMATE_MODE_LBL[CLM_COUNT] = {"Off", "Heat", "Cool", "Dry", "Fan", "Auto"};
// The mode button cycles through this subset (the everyday modes for a bedroom
// mini-split); other modes still display correctly if HA reports them.
static const int CLIMATE_MODE_CYCLE[] = {CLM_OFF, CLM_COOL, CLM_HEAT, CLM_AUTO};
static const int CLIMATE_MODE_CYCLE_N = sizeof(CLIMATE_MODE_CYCLE) / sizeof(CLIMATE_MODE_CYCLE[0]);

// Fan table (auto/low/medium/high). HA fan names that aren't in this list are
// mapped to the nearest entry in on_climate_fan_.
static const char *const CLIMATE_FAN_HA[] = {"auto", "low", "medium", "high"};
static const char *const CLIMATE_FAN_LBL[] = {"Auto", "Low", "Med", "High"};
static const int CLIMATE_FAN_N = 4;

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
static void ev_gesture(lv_event_t *e) {  // swipe up anywhere -> jump to the clock tile
  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr)
    return;
  if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP)
    static_cast<PatioUI *>(lv_event_get_user_data(e))->go_home_tile();
}
// lv_indev_scroll_throw_predict lives in src/indev/lv_indev_scroll.h, which
// lvgl.h does not pull in; forward-declare the public symbol.
extern "C" int32_t lv_indev_scroll_throw_predict(lv_indev_t *indev, lv_dir_t dir);
static void ev_scroll_begin(lv_event_t *e) {  // retune the snap animation to the fling velocity
  // LVGL hands us the scroll snap animation (created but not yet started) as the
  // event param. By default its duration is a fixed, distance-based 200-400 ms,
  // so every swipe snaps at the same speed regardless of how hard you flick.
  // Rewrite the duration from the fling velocity: hard flick -> quick snap,
  // gentle swipe -> slow ease. One tile per swipe is unchanged (tileview snap).
  lv_anim_t *a = static_cast<lv_anim_t *>(lv_event_get_param(e));
  if (a == nullptr)
    return;  // non-animated scroll
  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr)
    return;  // programmatic tile change (no finger) -> keep LVGL's default timing
  // Predicted throw distance is proportional to release velocity; take the
  // magnitude on whichever axis is scrolling as the "how hard did you flick" signal.
  int32_t pv = lv_indev_scroll_throw_predict(indev, LV_DIR_VER);
  int32_t ph = lv_indev_scroll_throw_predict(indev, LV_DIR_HOR);
  int32_t mag = LV_MAX(LV_ABS(pv), LV_ABS(ph));
  constexpr int32_t kFastMs = 120, kSlowMs = 400, kMagMax = 140;
  int32_t mag_c = mag > kMagMax ? kMagMax : mag;
  int32_t t = kSlowMs - (kSlowMs - kFastMs) * mag_c / kMagMax;
  lv_anim_set_duration(a, static_cast<uint32_t>(t));
}
static void ev_wake(lv_event_t *e) {  // tap while asleep -> wake, swallow the tap
  static_cast<PatioUI *>(lv_event_get_user_data(e))->wake_screen();
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
static void ev_climate_up(lv_event_t *e) {  // thermostat "+" -> raise setpoint one step
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_climate_setpoint(+1);
}
static void ev_climate_down(lv_event_t *e) {  // thermostat "-" -> lower setpoint one step
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_climate_setpoint(-1);
}
static void ev_climate_mode(lv_event_t *e) {  // mode dropdown -> set hvac mode
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_climate_mode_index(lv_dropdown_get_selected(dd));
}
static void ev_climate_fan(lv_event_t *e) {  // fan dropdown -> set fan mode
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  static_cast<PatioUI *>(lv_event_get_user_data(e))->request_climate_fan_index(lv_dropdown_get_selected(dd));
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

  // Tile scroll axis. Vertical was trialed (it hides the horizontal-shear
  // tearing better) but horizontal is the preferred feel for this hub. The
  // velocity-scaled snap (ev_scroll_begin) works in either orientation.
  static constexpr bool kVerticalTiles = false;

  lv_obj_t *tv = lv_tileview_create(scr);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);  // replaced by page dots below
  this->tv_ = tv;

  // Build only the configured tiles, in the configured order. add_tile() (called
  // from codegen) populated tile_order_/num_tiles_; if the YAML omitted `tiles:`
  // fall back to the historical patio set so the device is never blank.
  if (this->num_tiles_ == 0) {
    static const uint8_t kDefault[] = {TK_TIME, TK_HEATER, TK_LIGHTS, TK_SCREENS, TK_MEDIA};
    for (uint8_t k : kDefault)
      this->tile_order_[this->num_tiles_++] = k;
  }

  for (int col = 0; col < this->num_tiles_; col++) {
    lv_obj_t *tile = kVerticalTiles ? lv_tileview_add_tile(tv, 0, col, LV_DIR_VER)
                                    : lv_tileview_add_tile(tv, col, 0, LV_DIR_HOR);
    this->tiles_[col] = tile;
    // Bubble touch gestures up to the tileview/screen so a swipe on any tile
    // body reaches ev_gesture (attached to the screen below).
    lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
    switch (this->tile_order_[col]) {
      case TK_TIME:
        this->time_tile_ = tile;
        this->build_time_tile_(tile);
        break;
      case TK_HEATER:
        this->heater_tile_ = tile;
        this->build_heater_tile_(tile);
        break;
      case TK_CLIMATE:
        this->climate_tile_ = tile;
        this->build_climate_tile_(tile);
        break;
      case TK_LIGHTS:
        this->build_lights_tile_(tile);
        break;
      case TK_SCREENS:
        this->build_screens_tile_(tile);
        break;
      case TK_MEDIA:
        this->build_media_tile_(tile);
        break;
      default:
        break;
    }
  }
  lv_obj_add_flag(tv, LV_OBJ_FLAG_EVENT_BUBBLE);

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
  for (int i = 0; i < this->num_tiles_; i++) {
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
  lv_obj_add_event_cb(tv, ev_scroll_begin, LV_EVENT_SCROLL_BEGIN, this);  // velocity-scaled snap speed
  if (!kVerticalTiles)  // swipe-up==scroll in vertical mode; don't hijack it for go-home
    lv_obj_add_event_cb(scr, ev_gesture, LV_EVENT_GESTURE, this);
  this->update_page_dots_();

  // Full-screen presser on the top layer. Hidden while awake (so it blocks
  // nothing); revealed when the screen sleeps so the first touch lands here —
  // waking the screen without also actuating whatever was underneath. Kept
  // transparent: the visible black is drawn by the two eyelid bars below.
  this->wake_eater_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(this->wake_eater_);
  lv_obj_set_size(this->wake_eater_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(this->wake_eater_, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(this->wake_eater_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(this->wake_eater_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(this->wake_eater_, ev_wake, LV_EVENT_PRESSED, this);

  // Two opaque black "eyelids" on the top layer that close from the top and
  // bottom edges toward the centre to sleep the screen (heights animated in
  // fade_step_cb_). Non-clickable so taps fall through to the wake-eater.
  for (lv_obj_t **lid : {&this->eyelid_top_, &this->eyelid_bottom_}) {
    *lid = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(*lid);
    lv_obj_set_width(*lid, LV_PCT(100));
    lv_obj_set_height(*lid, 0);
    lv_obj_set_pos(*lid, 0, 0);
    lv_obj_set_style_bg_color(*lid, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(*lid, LV_OPA_COVER, 0);
    lv_obj_clear_flag(*lid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(*lid, LV_OBJ_FLAG_HIDDEN);
  }

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
  for (int i = 0; i < this->num_tiles_; i++) {
    if (this->page_dots_[i] == nullptr)
      continue;
    bool on = (this->tiles_[i] == active);
    lv_obj_set_style_bg_opa(this->page_dots_[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
}

// YAML codegen entry point: append a tile kind to the ordered set (bounded).
void PatioUI::add_tile(int kind) {
  if (this->num_tiles_ >= MAX_TILES)
    return;
  this->tile_order_[this->num_tiles_++] = static_cast<uint8_t>(kind);
}

// --- heater tile (live, wired to HA): iOS-timer style picker ---
//   idle    : scroll the roller to pick 15/30/45/60 min; Start begins the run
//   running : big MM:SS countdown; Cancel stops, "+15 min" extends the run
void PatioUI::build_heater_tile_(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, COL_HEATER, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  make_tile_title(tile, "Heater");

  // Vertical scroll picker (idle only). Options map 0..3 -> 15/30/45/60 min.
  lv_obj_t *roller = lv_roller_create(tile);
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
  this->heater_value_ = lv_label_create(tile);
  lv_obj_set_style_text_color(this->heater_value_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->heater_value_, &patio_font_countdown, 0);
  lv_label_set_text(this->heater_value_, "--");
  lv_obj_align(this->heater_value_, LV_ALIGN_TOP_MID, 0, 60);

  // Bottom action row: End Now (left, running only) + Start / "+15 min" (right).
  this->heater_btn_left_ = make_btn(tile, "End Now", ev_heater_cancel, this);
  lv_obj_set_size(this->heater_btn_left_, 132, 48);
  lv_obj_align(this->heater_btn_left_, LV_ALIGN_BOTTOM_LEFT, 14, -22);
  lv_obj_set_style_bg_color(this->heater_btn_left_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->heater_btn_left_, LV_OPA_30, 0);

  this->heater_btn_right_ = make_btn(tile, "Start", ev_heater_action, this);
  lv_obj_set_size(this->heater_btn_right_, 132, 48);
  lv_obj_align(this->heater_btn_right_, LV_ALIGN_BOTTOM_RIGHT, -14, -22);
  lv_obj_set_style_bg_color(this->heater_btn_right_, lv_color_hex(0xFFB870), 0);
  lv_obj_set_style_bg_opa(this->heater_btn_right_, LV_OPA_COVER, 0);
  this->heater_btn_right_lbl_ = lv_obj_get_child(this->heater_btn_right_, 0);
  lv_obj_set_style_text_color(this->heater_btn_right_lbl_, lv_color_black(), 0);
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

  // Top-corner status icons. Both start hidden and are only shown in their
  // alert state by refresh_status_icons_(): the WiFi glyph (top-left, red)
  // appears only when the network is down; the battery glyph + % (top-right)
  // appears only while the unit is running on battery.
  this->status_wifi_ = lv_label_create(tile);
  lv_label_set_text(this->status_wifi_, PATIO_WIFI_OFF_SYMBOL);
  lv_obj_set_style_text_font(this->status_wifi_, &patio_font_wifi_off, 0);
  lv_obj_set_style_text_color(this->status_wifi_, lv_color_hex(0xFF5555), 0);
  lv_obj_align(this->status_wifi_, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_add_flag(this->status_wifi_, LV_OBJ_FLAG_HIDDEN);

  this->status_batt_ = lv_label_create(tile);
  lv_label_set_text(this->status_batt_, LV_SYMBOL_BATTERY_FULL);
  lv_obj_set_style_text_font(this->status_batt_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(this->status_batt_, lv_color_white(), 0);
  lv_obj_align(this->status_batt_, LV_ALIGN_TOP_RIGHT, -6, 6);
  lv_obj_add_flag(this->status_batt_, LV_OBJ_FLAG_HIDDEN);
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
  this->refresh_status_icons_();
}

// Update the clock-tile status icons from the polled atomics. LVGL task only.
void PatioUI::refresh_status_icons_() {
  if (this->status_wifi_ != nullptr) {
    if (this->wifi_up_.load())
      lv_obj_add_flag(this->status_wifi_, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(this->status_wifi_, LV_OBJ_FLAG_HIDDEN);
  }
  if (this->status_batt_ != nullptr) {
    if (!this->on_battery_.load()) {
      lv_obj_add_flag(this->status_batt_, LV_OBJ_FLAG_HIDDEN);
    } else {
      int pct = this->batt_pct_.load();
      const char *sym;
      if (pct < 0)
        sym = LV_SYMBOL_BATTERY_EMPTY;
      else if (pct >= 90)
        sym = LV_SYMBOL_BATTERY_FULL;
      else if (pct >= 65)
        sym = LV_SYMBOL_BATTERY_3;
      else if (pct >= 40)
        sym = LV_SYMBOL_BATTERY_2;
      else if (pct >= 15)
        sym = LV_SYMBOL_BATTERY_1;
      else
        sym = LV_SYMBOL_BATTERY_EMPTY;
      char buf[24];
      if (pct >= 0)
        snprintf(buf, sizeof(buf), "%s %d%%", sym, pct);
      else
        snprintf(buf, sizeof(buf), "%s", sym);
      lv_label_set_text(this->status_batt_, buf);
      lv_obj_set_style_text_color(this->status_batt_,
                                  (pct >= 0 && pct < 15) ? lv_color_hex(0xFF5555) : lv_color_white(), 0);
      lv_obj_clear_flag(this->status_batt_, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// Main task: read power/battery from the AXP2101 and the network state, stash
// into atomics for the LVGL task. Runs throttled from loop().
void PatioUI::poll_power_status_() {
  this->wifi_up_.store(network::is_connected());

  if (this->axp_dev_ == nullptr)
    return;
  auto dev = static_cast<i2c_master_dev_handle_t>(this->axp_dev_);
  uint8_t reg, val;
  int raw_status = -1, raw_gauge = -1, mv = -1;

  // STATUS1 (0x00) bit5 = VBUS present. On battery == no VBUS.
  reg = 0x00;
  if (i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 200) == ESP_OK) {
    raw_status = val;
    this->on_battery_.store((val & 0x20) == 0);
  }

  // Fuel-gauge percentage (reg 0xA4). Some units leave the gauge disabled and
  // return 0/255 — fall back to a voltage-based estimate in that case.
  reg = 0xA4;
  if (i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 200) == ESP_OK)
    raw_gauge = val;

  // Battery voltage (regs 0x34/0x35, H6L8 -> mV) as the fallback source.
  {
    uint8_t rh = 0x34, rl = 0x35, vh = 0, vl = 0;
    if (i2c_master_transmit_receive(dev, &rh, 1, &vh, 1, 200) == ESP_OK &&
        i2c_master_transmit_receive(dev, &rl, 1, &vl, 1, 200) == ESP_OK)
      mv = ((vh & 0x3F) << 8) | vl;
  }

  int pct = -1;
  if (raw_gauge >= 1 && raw_gauge <= 100)
    pct = raw_gauge;
  else if (mv > 2000)
    pct = PatioUI::batt_mv_to_pct_(mv);
  if (pct >= 0)
    this->batt_pct_.store(pct);

  ESP_LOGD(TAG, "power: net=%d status1=0x%02X on_batt=%d gauge=%d mv=%d pct=%d",
           (int) this->wifi_up_.load(), raw_status, (int) this->on_battery_.load(), raw_gauge, mv,
           this->batt_pct_.load());
}

// Rough single-cell LiPo state-of-charge from resting voltage (mV). Only used
// when the AXP fuel gauge (reg 0xA4) is unavailable.
int PatioUI::batt_mv_to_pct_(int mv) {
  static const int lut[][2] = {
      {4200, 100}, {4100, 90}, {4000, 78}, {3900, 63}, {3800, 48},
      {3700, 32},  {3600, 18}, {3500, 8},  {3300, 0},
  };
  const int n = sizeof(lut) / sizeof(lut[0]);
  if (mv >= lut[0][0])
    return 100;
  if (mv <= lut[n - 1][0])
    return 0;
  for (int i = 0; i < n - 1; i++) {
    int hv = lut[i][0], hp = lut[i][1];
    int lv = lut[i + 1][0], lp = lut[i + 1][1];
    if (mv <= hv && mv >= lv)
      return lp + (mv - lv) * (hp - lp) / (hv - lv);
  }
  return 0;
}

// Runtime idle-timeout setters (called from HA number set_action / on_boot on
// the main task; only touch atomics). Values arrive in seconds; clamp to sane
// bounds and store as ms. sleep must be >= screen or deep sleep would fire the
// instant the screen cuts (we floor the delta at 0 in maybe_deep_sleep_).
void PatioUI::set_screen_timeout_s(float s) {
  if (!(s > 0))  // guard NaN / non-positive
    return;
  if (s < 5)
    s = 5;
  this->screen_sleep_ms_.store(static_cast<uint32_t>(s * 1000.0f + 0.5f));
  ESP_LOGI(TAG, "screen timeout -> %.0f s", s);
}

void PatioUI::set_sleep_timeout_s(float s) {
  if (!(s > 0))
    return;
  if (s < 5)
    s = 5;
  this->deep_sleep_total_ms_.store(static_cast<uint32_t>(s * 1000.0f + 0.5f));
  ESP_LOGI(TAG, "sleep (deep-sleep) timeout -> %.0f s", s);
}

// Main task: on battery, once the screen has been asleep (backlight cut) for
// DEEP_SLEEP_AFTER_SCREEN_MS of additional idle time, hand off to the ESPHome
// deep_sleep component (armed by the board entrypoint with GPIO39 tap-wake).
// Never fires on USB power — plugging in keeps the device awake/reachable. A
// tap on the dark screen pulls the FT6336 INT (RTC-capable GPIO39) low and
// reboots the device straight back into the UI. Called from loop() just after
// the power poll so on_battery_ is fresh.
void PatioUI::maybe_deep_sleep_() {
  uint32_t at = this->screen_asleep_at_ms_.load();
  if (at == 0) {
    this->deep_sleep_pending_ = false;   // screen awake again -> re-arm for next time
    return;
  }
  if (this->deep_sleep_pending_)
    return;                              // already handed off; awaiting sleep
  if (!this->on_battery_.load())
    return;                              // on USB -> never deep sleep
  // Additional idle after the screen slept = total idle target minus the screen
  // timeout (floored at 0). Both are runtime-adjustable from HA.
  uint32_t screen_ms = this->screen_sleep_ms_.load();
  uint32_t total_ms = this->deep_sleep_total_ms_.load();
  uint32_t additional = (total_ms > screen_ms) ? (total_ms - screen_ms) : 0;
  if ((millis() - at) < additional)
    return;                              // not idle long enough yet
  this->deep_sleep_pending_ = true;
  ESP_LOGI(TAG, "on battery + screen idle -> entering deep sleep (tap GPIO39 to wake)");
  this->deep_sleep_trigger_.trigger();
}

// After IDLE_REVERT_MS with no touch, drift back to the clock tile — unless a
// heater timer is running, in which case rest on the heater/countdown tile so
// the remaining time stays visible. LVGL task only.
void PatioUI::maybe_auto_revert_() {
  if (this->tv_ == nullptr)
    return;
  if (lv_display_get_inactive_time(nullptr) < IDLE_REVERT_MS)
    return;
  // Rest on the heater/countdown tile while a timer runs (so remaining time
  // stays visible), otherwise the clock tile. Fall back gracefully when either
  // tile isn't in the configured set.
  lv_obj_t *target = (this->active_.load() && this->heater_tile_ != nullptr)
                         ? this->heater_tile_
                         : this->time_tile_;
  if (target == nullptr)
    return;
  if (lv_tileview_get_tile_active(this->tv_) == target)
    return;
  lv_tileview_set_tile(this->tv_, target, LV_ANIM_ON);
  this->update_page_dots_();
}

// After SCREEN_SLEEP_MS untouched, close two black "eyelids" from the top and
// bottom edges toward the centre, then cut the backlight — avoids burn-in from
// the static clock. The top-layer presser is revealed up front so it swallows
// the next touch (which only wakes). Inhibited while a heater timer is running
// (the countdown changes constantly — no burn-in risk — and the remaining time
// should stay visible). LVGL task only.
void PatioUI::maybe_screen_sleep_() {
  if (this->wake_eater_ == nullptr || this->eyelid_top_ == nullptr || this->screen_asleep_)
    return;
  if (this->active_.load())
    return;
  if (lv_display_get_inactive_time(nullptr) < this->screen_sleep_ms_.load())
    return;
  this->screen_asleep_ = true;
  lv_obj_set_height(this->eyelid_top_, 0);
  lv_obj_set_height(this->eyelid_bottom_, 0);
  for (lv_obj_t *o : {this->wake_eater_, this->eyelid_top_, this->eyelid_bottom_}) {
    lv_obj_move_foreground(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  this->sleep_kf_idx_ = 0;
  this->start_sleep_kf_(0);
  ESP_LOGD(TAG, "screen getting drowsy (eyelids fighting it)");
}

// One segment of the "fighting sleep" close: animate both eyelids from their
// current height to keyframe #idx's target, then hand off to sleep_kf_done_cb_
// which advances to the next segment. The table droops the lids partway and
// lets them spring back a couple of times (a groggy "nodding off" struggle)
// before the final full close. Targets are a permille fraction of the
// half-screen height so they scale with the display. When the table is
// exhausted the eyelids are shut -> finish_sleep_(). LVGL task only.
void PatioUI::start_sleep_kf_(int idx) {
  struct KF {
    int16_t frac_permille;   // eyelid height as permille of half-screen (0=open, 1000=shut)
    uint16_t dur_ms;
    lv_anim_path_cb_t path;  // easing for this segment
  };
  static const KF kf[] = {
      {500, 620, lv_anim_path_ease_in},   // slow droop — starting to give in
      {120, 150, lv_anim_path_ease_out},  // fast jerk back awake
      {120, 320, lv_anim_path_linear},    // ...hold, fighting to stay awake
      {800, 780, lv_anim_path_ease_in},   // heavier, slower droop
      {250, 160, lv_anim_path_ease_out},  // fast jerk awake again
      {250, 240, lv_anim_path_linear},    // one last brief hold
      {1000, 720, lv_anim_path_ease_in},  // can't win — slow final close
  };
  static const int kf_n = sizeof(kf) / sizeof(kf[0]);
  if (idx >= kf_n) {
    this->finish_sleep_();
    return;
  }
  const int32_t half = (lv_display_get_vertical_resolution(nullptr) + 1) / 2;  // meet, no seam
  const int32_t from = lv_obj_get_height(this->eyelid_top_);
  const int32_t to = (int32_t) half * kf[idx].frac_permille / 1000;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, this);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, kf[idx].dur_ms);
  lv_anim_set_exec_cb(&a, PatioUI::fade_step_cb_);
  lv_anim_set_completed_cb(&a, PatioUI::sleep_kf_done_cb_);
  lv_anim_set_path_cb(&a, kf[idx].path);
  lv_anim_start(&a);
}

// lv_anim step: grow the two eyelids toward the centre (var == this). The top
// lid extends down from y=0; the bottom lid's top edge tracks up from the
// bottom edge so it grows upward.
void PatioUI::fade_step_cb_(void *var, int32_t v) {
  auto *self = static_cast<PatioUI *>(var);
  const int32_t h = lv_display_get_vertical_resolution(nullptr);
  if (self->eyelid_top_ != nullptr) {
    lv_obj_set_pos(self->eyelid_top_, 0, 0);
    lv_obj_set_height(self->eyelid_top_, v);
  }
  if (self->eyelid_bottom_ != nullptr) {
    lv_obj_set_height(self->eyelid_bottom_, v);
    lv_obj_set_pos(self->eyelid_bottom_, 0, h - v);
  }
}

// lv_anim completed for one close segment: advance to the next keyframe. When
// the table runs out, start_sleep_kf_ calls finish_sleep_.
void PatioUI::sleep_kf_done_cb_(lv_anim_t *a) {
  auto *self = static_cast<PatioUI *>(a->var);
  self->start_sleep_kf_(++self->sleep_kf_idx_);
}

// Eyelids are fully shut: cut the backlight rail and drop the CPU floor to
// 80 MHz (ceiling stays 240 so OTA/API bursts still ramp up). WiFi keeps the
// APB clock locked at 80 MHz, so peripheral timing and touch-wake are
// unaffected. LVGL task.
void PatioUI::finish_sleep_() {
  this->set_backlight_rail_(false);
#ifdef PATIO_AUDIO
  // CoreS3 voice hub: USB-powered and must stay responsive to the always-on
  // wake word + native-API keepalive. Dropping the CPU floor to 80 MHz starves
  // the Noise-encryption handshake/keepalive, so HA marks the device
  // "unresponsive" and the satellite goes unavailable the moment the screen
  // sleeps. Keep the clock pinned at full speed even while the screen is off
  // (we still cut the backlight for burn-in). Power-save is irrelevant on USB.
  this->set_cpu_freq_(240, 240);
#else
  this->set_cpu_freq_(80, 240);
#endif
  this->screen_asleep_at_ms_.store(millis());  // arm the on-battery deep-sleep timer
  ESP_LOGD(TAG, "screen asleep (backlight off)");
}

// Retained no-op removed: the single-shot close hook was replaced by the
// keyframe nod-off sequence above (sleep_kf_done_cb_ / finish_sleep_).

// Wake from screen-sleep: back to full speed, backlight on (the shut eyelids
// still cover the panel), then a single deliberate "eyes open" sweep retracts
// them to the edges. wake_done_cb_ hides the overlays once fully open. The
// wake-eater stays up during the sweep so the waking tap (and any taps mid-open)
// are swallowed rather than actuating a control underneath. LVGL task only
// (called from the presser's press event).
void PatioUI::wake_screen() {
  if (!this->screen_asleep_)
    return;
  this->screen_asleep_ = false;                    // re-entrant taps now no-op
  this->screen_asleep_at_ms_.store(0);             // disarm the deep-sleep timer
  lv_anim_delete(this, PatioUI::fade_step_cb_);    // stop the nod-off if still closing
  this->set_cpu_freq_(240, 240);                   // full speed before the redraw
  this->set_backlight_rail_(true);                 // rail on; eyelids still hide the panel
  lv_display_trigger_activity(nullptr);            // restart the idle clock from the wake
  const int32_t from = lv_obj_get_height(this->eyelid_top_);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, this);
  lv_anim_set_values(&a, from, 0);
  lv_anim_set_duration(&a, SCREEN_OPEN_MS);
  lv_anim_set_exec_cb(&a, PatioUI::fade_step_cb_);
  lv_anim_set_completed_cb(&a, PatioUI::wake_done_cb_);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);  // decisive, deliberate open
  lv_anim_start(&a);
  ESP_LOGD(TAG, "screen waking (eyelids opening)");
}

// lv_anim completed for the wake sweep: eyelids are fully open, so hide the
// wake-eater + eyelids and let normal touches through again. LVGL task.
void PatioUI::wake_done_cb_(lv_anim_t *a) {
  auto *self = static_cast<PatioUI *>(a->var);
  for (lv_obj_t *o : {self->wake_eater_, self->eyelid_top_, self->eyelid_bottom_})
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  ESP_LOGD(TAG, "screen awake (backlight on)");
}

// Configure CPU dynamic-frequency scaling via the ESP-IDF power-management
// framework (DFS, no light sleep). Pass a floor and ceiling: DFS idles at
// min_mhz and automatically ramps toward max_mhz under load. Awake we pin
// 240/240; asleep we use 80/240 so the CPU idles low to save power but can
// still ramp to full speed for bursty work (OTA, API/HA pushes, WiFi RX) --
// pinning max to 80 while asleep is what previously stalled OTA. 80 MHz is the
// floor that keeps APB at 80 MHz so WiFi and I2C/SPI/UART timing stay correct.
// Requires CONFIG_PM_ENABLE (set in the YAML sdkconfig_options).
void PatioUI::set_cpu_freq_(int min_mhz, int max_mhz) {
  esp_pm_config_t pm{};
  pm.max_freq_mhz = max_mhz;
  pm.min_freq_mhz = min_mhz;
  pm.light_sleep_enable = false;
  esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "esp_pm_configure(%d-%d MHz) failed: %s", min_mhz, max_mhz, esp_err_to_name(err));
  else
    ESP_LOGD(TAG, "cpu freq set to %d-%d MHz", min_mhz, max_mhz);
}

// Cut or restore the LCD backlight. bsp_display_backlight_off() disables the
// AXP2101 BLDO1 backlight rail (esp-bsp Core2 v1.1 fix), fully darkening the
// panel — merely setting brightness to 0 leaves it at a ~2.5 V floor and
// faintly lit. bsp_display_brightness_set(>0) re-enables the rail, so restoring
// our normal 80% brightness also brings the backlight back. LVGL task.
void PatioUI::set_backlight_rail_(bool on) {
  if (on)
    bsp_display_brightness_set(80);
  else
    bsp_display_backlight_off();
}

// Swipe-up gesture -> jump straight back to the clock (first) tile. LVGL task only.
void PatioUI::go_home_tile() {
  lv_obj_t *home = (this->time_tile_ != nullptr) ? this->time_tile_ : this->tiles_[0];
  if (this->tv_ == nullptr || home == nullptr)
    return;
  if (lv_tileview_get_tile_active(this->tv_) == home)
    return;
  lv_tileview_set_tile(this->tv_, home, LV_ANIM_ON);
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
  if (this->heater_value_ == nullptr || this->heater_tile_ == nullptr)
    return;
  int s = this->countdown_secs_.load();
  bool in_window = this->active_.load() && s > 0 && s <= EXPIRY_FLASH_SECS;
  if (in_window) {
    this->flash_on_ = !this->flash_on_;
    lv_color_t bg = this->flash_on_ ? lv_color_white() : COL_FLASH;
    lv_color_t fg = this->flash_on_ ? COL_FLASH : lv_color_white();
    lv_obj_set_style_bg_color(this->heater_tile_, bg, 0);
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
  if (this->climate_ui_dirty_.exchange(false))
    this->refresh_climate_ui_();
  if (this->screen_ui_dirty_.exchange(false))
    this->update_screen_visual_();
  // Clock tile refresh + idle auto-revert (both LVGL-task safe).
  this->refresh_time_tile_();
  this->maybe_auto_revert_();
  this->maybe_screen_sleep_();
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
  if (this->heater_tile_ != nullptr && !this->flashing_) {
    lv_color_t bg = active ? heater_bg_for_remaining(this->countdown_secs_.load()) : COL_HEATER;
    lv_obj_set_style_bg_color(this->heater_tile_, bg, 0);
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
  lv_label_set_text(hdr, this->screen_title_.c_str());
  lv_obj_set_style_text_color(hdr, lv_color_white(), 0);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);

  for (int i = 0; i < NUM_SCREENS; i++) {
    this->screen_tap_[i].self = this;
    this->screen_tap_[i].idx = i;
  }

  if (this->screen_layout_ == 1) {
    // Horizontal row layout (e.g. bedroom blinds): the configured covers sit in
    // a single left-to-right row. Side slots (left/right) are single-width; the
    // rear slots (used as the "centre" positions) are double-width, so a
    // left/centre/right trio reads as narrow | wide | narrow. All use the rear
    // (horizontal headrail) button style so they read as roller blinds.
    static const int kRowOrder[NUM_SCREENS] = {0, 2, 3, 1};   // left, rear_left, rear_right, right
    static const int kRowWeight[NUM_SCREENS] = {1, 1, 2, 2};  // by slot: sides x1, rears x2

    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 304, 124);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    for (int k = 0; k < NUM_SCREENS; k++) {
      int slot = kRowOrder[k];
      if (!this->screen_configured_[slot])
        continue;
      lv_obj_t *b = make_screen_button(row, &this->screen_tap_[slot], SCR_REAR);
      lv_obj_set_height(b, 116);
      lv_obj_set_width(b, 0);                          // flex-basis 0 -> pure proportional
      lv_obj_set_flex_grow(b, kRowWeight[slot]);       // sides:1, centre:2 -> 1:2:1
      this->screen_btn_[slot] = b;
    }
  } else {
    // Perimeter map (patio deck): side screens on the edges, rear pair centred.
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
  }

  // Position-fill overlay: a translucent "shade" drawn over each blind's window
  // pane, growing downward from the top in proportion to how far the blind is
  // lowered (100 - current_position). Hidden until HA reports a position, so
  // feedback-less covers (Somfy RTS) simply never show it. The pane is the
  // button's first child (see make_screen_button).
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (!this->screen_configured_[i] || this->screen_btn_[i] == nullptr)
      continue;
    lv_obj_t *pane = lv_obj_get_child(this->screen_btn_[i], 0);
    if (pane == nullptr)
      continue;
    lv_obj_t *fill = lv_obj_create(pane);
    lv_obj_remove_style_all(fill);
    lv_obj_set_width(fill, LV_PCT(100));
    lv_obj_set_height(fill, LV_PCT(0));
    lv_obj_align(fill, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0xE6D8B8), 0);  // warm shade fabric
    lv_obj_set_style_bg_opa(fill, LV_OPA_70, 0);
    lv_obj_set_style_radius(fill, 3, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(fill, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    this->screen_fill_[i] = fill;
  }

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
    else if (this->screen_btn_[i] != nullptr)  // row layout leaves gaps unbuilt
      lv_obj_add_flag(this->screen_btn_[i], LV_OBJ_FLAG_HIDDEN);
  }

  this->update_screen_visual_();
}

// ---------------- selection highlight + control state (LVGL task) ----------------
void PatioUI::update_screen_visual_() {
  bool any_sel = false;
  bool any_unknown_sel = false;  // a selected cover with no position feedback
  bool all_open = true;          // every selected+known cover fully open (100)
  bool all_closed = true;        // every selected+known cover fully closed (0)
  int known_sel = 0;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (!this->screen_configured_[i])
      continue;
    lv_obj_t *b = this->screen_btn_[i];
    if (b == nullptr)
      continue;

    // Position-fill overlay: shade drops from the top by (100 - position)%.
    int pos = this->screen_pos_[i].load();
    if (this->screen_fill_[i] != nullptr) {
      if (pos < 0) {
        lv_obj_add_flag(this->screen_fill_[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        int down = 100 - pos;  // how far the blind is lowered
        lv_obj_set_height(this->screen_fill_[i], LV_PCT(down));
        if (down <= 0)
          lv_obj_add_flag(this->screen_fill_[i], LV_OBJ_FLAG_HIDDEN);
        else
          lv_obj_clear_flag(this->screen_fill_[i], LV_OBJ_FLAG_HIDDEN);
      }
    }

    if (this->screen_sel_[i]) {
      any_sel = true;
      if (pos < 0) {
        any_unknown_sel = true;
      } else {
        known_sel++;
        if (pos < 100)
          all_open = false;
        if (pos > 0)
          all_closed = false;
      }
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

  // Enablement: Stop is live whenever something is selected. Up/Down additionally
  // disable at the travel limit — but only when every selected cover has known
  // position (a selected feedback-less cover, -1, keeps them live).
  bool limit_known = any_sel && !any_unknown_sel && known_sel > 0;
  bool up_en = any_sel && !(limit_known && all_open);
  bool down_en = any_sel && !(limit_known && all_closed);
  bool en[3] = {up_en, any_sel, down_en};  // up, stop, down
  lv_obj_t *ctrls[3] = {this->ctrl_up_, this->ctrl_stop_, this->ctrl_down_};
  for (int i = 0; i < 3; i++) {
    if (ctrls[i] == nullptr)
      continue;
    if (en[i]) {
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
  lv_obj_set_size(this->media_vol_slider_, 189, 12);
  lv_obj_align(this->media_vol_slider_, LV_ALIGN_TOP_LEFT, 51, 108);
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

// ================= climate (thermostat) tile =================
// A big centred setpoint with -/+ steppers, current-temperature readout, and
// mode/fan selector dropdowns along the bottom. Wired to a HA climate entity;
// both directions are live (HA changes flow back via refresh_climate_ui_()).
void PatioUI::build_climate_tile_(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, COL_CLIMATE_OFF, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  make_tile_title(tile, this->climate_label_.c_str());

  // Current-temperature readout under the title ("Now 76°").
  this->climate_cur_lbl_ = lv_label_create(tile);
  lv_obj_set_style_text_color(this->climate_cur_lbl_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->climate_cur_lbl_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_opa(this->climate_cur_lbl_, LV_OPA_80, 0);
  lv_label_set_text(this->climate_cur_lbl_, this->climate_celsius_ ? "Now --\xC2\xB0" "C" : "Now --\xC2\xB0" "F");
  lv_obj_align(this->climate_cur_lbl_, LV_ALIGN_TOP_MID, 0, 36);

  // Big centred setpoint ("68°"). montserrat_48 carries the degree glyph.
  this->climate_set_lbl_ = lv_label_create(tile);
  lv_obj_set_style_text_color(this->climate_set_lbl_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->climate_set_lbl_, &lv_font_montserrat_48, 0);
  lv_label_set_text(this->climate_set_lbl_, this->climate_celsius_ ? "--\xC2\xB0" "C" : "--\xC2\xB0" "F");
  lv_obj_align(this->climate_set_lbl_, LV_ALIGN_CENTER, 0, -18);

  // Big round -/+ steppers flanking the setpoint. Use the built-in symbol
  // glyphs (montserrat lacks the U+2212 minus, which rendered as a box).
  lv_obj_t *b_down = make_btn(tile, LV_SYMBOL_MINUS, ev_climate_down, this);
  lv_obj_set_size(b_down, 66, 66);
  lv_obj_set_style_radius(b_down, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_font(lv_obj_get_child(b_down, 0), &lv_font_montserrat_28, 0);
  lv_obj_align(b_down, LV_ALIGN_LEFT_MID, 12, -18);

  lv_obj_t *b_up = make_btn(tile, LV_SYMBOL_PLUS, ev_climate_up, this);
  lv_obj_set_size(b_up, 66, 66);
  lv_obj_set_style_radius(b_up, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_font(lv_obj_get_child(b_up, 0), &lv_font_montserrat_28, 0);
  lv_obj_align(b_up, LV_ALIGN_RIGHT_MID, -12, -18);

  // Bottom row: mode selector (left) + fan selector (right). Dropdowns so a
  // mode can be picked directly (Off is a normal choice, not a toggle stop).
  // Lists open upward — there's little room below on the tile.
  this->climate_mode_dd_ = lv_dropdown_create(tile);
  lv_dropdown_set_options(this->climate_mode_dd_, "Off\nCool\nHeat\nAuto");
  lv_dropdown_set_dir(this->climate_mode_dd_, LV_DIR_TOP);
  lv_dropdown_set_symbol(this->climate_mode_dd_, NULL);
  lv_obj_set_size(this->climate_mode_dd_, 138, 46);
  lv_obj_align(this->climate_mode_dd_, LV_ALIGN_BOTTOM_LEFT, 14, -30);
  lv_obj_set_style_bg_color(this->climate_mode_dd_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->climate_mode_dd_, LV_OPA_30, 0);
  lv_obj_set_style_border_width(this->climate_mode_dd_, 0, 0);
  lv_obj_set_style_text_color(this->climate_mode_dd_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->climate_mode_dd_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(this->climate_mode_dd_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_event_cb(this->climate_mode_dd_, ev_climate_mode, LV_EVENT_VALUE_CHANGED, this);

  this->climate_fan_dd_ = lv_dropdown_create(tile);
  lv_dropdown_set_options(this->climate_fan_dd_, "Auto\nLow\nMed\nHigh");
  lv_dropdown_set_dir(this->climate_fan_dd_, LV_DIR_TOP);
  lv_dropdown_set_symbol(this->climate_fan_dd_, NULL);
  lv_obj_set_size(this->climate_fan_dd_, 138, 46);
  lv_obj_align(this->climate_fan_dd_, LV_ALIGN_BOTTOM_RIGHT, -14, -30);
  lv_obj_set_style_bg_color(this->climate_fan_dd_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->climate_fan_dd_, LV_OPA_30, 0);
  lv_obj_set_style_border_width(this->climate_fan_dd_, 0, 0);
  lv_obj_set_style_text_color(this->climate_fan_dd_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->climate_fan_dd_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(this->climate_fan_dd_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_event_cb(this->climate_fan_dd_, ev_climate_fan, LV_EVENT_VALUE_CHANGED, this);

  // No entity bound -> show a disabled placeholder rather than dead controls.
  if (!this->climate_configured_) {
    lv_label_set_text(this->climate_cur_lbl_, "(no climate entity)");
    lv_obj_t *ctrls[4] = {b_down, b_up, this->climate_mode_dd_, this->climate_fan_dd_};
    for (auto *c : ctrls) {
      lv_obj_add_state(c, LV_STATE_DISABLED);
      lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_opa(c, LV_OPA_40, 0);
    }
  }
}

// Reflect last-known HA state onto the climate tile (LVGL task).
void PatioUI::refresh_climate_ui_() {
  if (!this->climate_configured_)
    return;
  int mode = this->climate_mode_.load();
  if (mode < 0 || mode >= CLM_COUNT)
    mode = CLM_OFF;

  // Tile background follows the mode (cool=blue, heat=orange, auto=teal, else slate).
  lv_color_t bg = COL_CLIMATE_OFF;
  if (mode == CLM_COOL || mode == CLM_DRY || mode == CLM_FAN)
    bg = COL_CLIMATE_COOL;
  else if (mode == CLM_HEAT)
    bg = COL_CLIMATE_HEAT;
  else if (mode == CLM_AUTO)
    bg = COL_CLIMATE_AUTO;
  lv_obj_set_style_bg_color(this->climate_tile_, bg, 0);

  // Current temperature line ("Now 76°C"), or hide the number when unknown.
  const char *unit = this->climate_celsius_ ? "C" : "F";
  if (this->climate_cur_lbl_ != nullptr) {
    int cx = this->climate_cur_x10_.load();
    char b[24];
    if (cx == -10000)
      snprintf(b, sizeof(b), "Now --\xC2\xB0%s", unit);
    else
      snprintf(b, sizeof(b), "Now %d\xC2\xB0%s", (cx + (cx >= 0 ? 5 : -5)) / 10, unit);
    lv_label_set_text(this->climate_cur_lbl_, b);
  }

  // Big setpoint. When off, show a dash instead of a stale number.
  if (this->climate_set_lbl_ != nullptr) {
    int tx = this->climate_target_x10_.load();
    char b[16];
    if (mode == CLM_OFF || tx == -10000)
      snprintf(b, sizeof(b), "--\xC2\xB0%s", unit);
    else if (this->climate_step_ < 0.99f && (tx % 10) != 0)
      snprintf(b, sizeof(b), "%d.%d\xC2\xB0%s", tx / 10, (tx % 10 < 0 ? -tx % 10 : tx % 10), unit);
    else
      snprintf(b, sizeof(b), "%d\xC2\xB0%s", (tx + (tx >= 0 ? 5 : -5)) / 10, unit);
    lv_label_set_text(this->climate_set_lbl_, b);
  }

  // Sync the selector dropdowns to the current mode/fan. lv_dropdown_set_selected
  // does not fire an event, so there's no feedback loop.
  if (this->climate_mode_dd_ != nullptr) {
    int sel = -1;
    for (int i = 0; i < CLIMATE_MODE_CYCLE_N; i++) {
      if (CLIMATE_MODE_CYCLE[i] == mode) {
        sel = i;
        break;
      }
    }
    // Modes outside the selectable set (dry/fan_only) leave the selection as-is;
    // the tile background still reflects the true mode above.
    if (sel >= 0)
      lv_dropdown_set_selected(this->climate_mode_dd_, sel);
  }
  if (this->climate_fan_dd_ != nullptr) {
    int fan = this->climate_fan_.load();
    if (fan < 0 || fan >= CLIMATE_FAN_N)
      fan = 0;
    lv_dropdown_set_selected(this->climate_fan_dd_, fan);
  }
}

// ---------------- climate intents (LVGL task) ----------------
void PatioUI::request_climate_setpoint(int delta_steps) {
  if (!this->climate_configured_)
    return;
  int step10 = static_cast<int>(this->climate_step_ * 10.0f + 0.5f);
  if (step10 <= 0)
    step10 = 10;
  int cur = this->climate_target_x10_.load();
  if (cur == -10000)  // no known setpoint yet — seed at the min
    cur = static_cast<int>(this->climate_min_ * 10.0f + 0.5f);
  int next = cur + delta_steps * step10;
  int lo = static_cast<int>(this->climate_min_ * 10.0f + 0.5f);
  int hi = static_cast<int>(this->climate_max_ * 10.0f + 0.5f);
  if (next < lo)
    next = lo;
  if (next > hi)
    next = hi;
  this->climate_target_x10_.store(next);           // optimistic
  this->pending_climate_target_x10_.store(next);   // main task sends it
  this->climate_ui_dirty_.store(true);
}

void PatioUI::request_climate_mode_index(int idx) {
  if (!this->climate_configured_)
    return;
  if (idx < 0 || idx >= CLIMATE_MODE_CYCLE_N)
    return;
  int mode = CLIMATE_MODE_CYCLE[idx];
  this->climate_mode_.store(mode);                 // optimistic
  this->pending_climate_mode_.store(mode);
  this->climate_ui_dirty_.store(true);
}

void PatioUI::request_climate_fan_index(int idx) {
  if (!this->climate_configured_)
    return;
  if (idx < 0 || idx >= CLIMATE_FAN_N)
    return;
  this->climate_fan_.store(idx);                   // optimistic
  this->pending_climate_fan_.store(idx);
  this->climate_ui_dirty_.store(true);
}

// ---------------- climate HA -> UI callbacks (main/API task) ----------------
void PatioUI::on_climate_state_(std::string state) {
  int mode = CLM_OFF;
  for (int i = 0; i < CLM_COUNT; i++) {
    if (state == CLIMATE_MODE_HA[i]) {
      mode = i;
      break;
    }
  }
  this->climate_mode_.store(mode);
  this->climate_ui_dirty_.store(true);
  ESP_LOGD(TAG, "climate mode -> %s (%d)", state.c_str(), mode);
}
void PatioUI::on_climate_action_(std::string action) {
  int a = 0;
  if (action == "heating")
    a = 1;
  else if (action == "cooling")
    a = 2;
  else if (action == "drying")
    a = 3;
  else if (action == "fan")
    a = 4;
  this->climate_action_.store(a);
  this->climate_ui_dirty_.store(true);
}
void PatioUI::on_climate_cur_temp_(std::string v) {
  char *end = nullptr;
  float f = strtof(v.c_str(), &end);
  if (end != v.c_str()) {
    if (this->climate_celsius_)
      f = (f - 32.0f) * 5.0f / 9.0f;
    this->climate_cur_x10_.store(static_cast<int>(f * 10.0f + (f >= 0 ? 0.5f : -0.5f)));
    this->climate_ui_dirty_.store(true);
  }
}
void PatioUI::on_climate_target_temp_(std::string v) {
  char *end = nullptr;
  float f = strtof(v.c_str(), &end);
  if (end != v.c_str()) {
    if (this->climate_celsius_)
      f = (f - 32.0f) * 5.0f / 9.0f;
    // Ignore the echo of a setpoint the user is mid-adjusting only if a send is
    // still pending; otherwise trust HA as the source of truth.
    if (this->pending_climate_target_x10_.load() == -10000)
      this->climate_target_x10_.store(static_cast<int>(f * 10.0f + (f >= 0 ? 0.5f : -0.5f)));
    this->climate_ui_dirty_.store(true);
  }
}
void PatioUI::on_climate_fan_(std::string v) {
  // Map HA fan names (which include diffuse/middle on this unit) to our 4 slots.
  int fan = 0;
  if (v == "low" || v == "diffuse")
    fan = 1;
  else if (v == "medium" || v == "middle")
    fan = 2;
  else if (v == "high")
    fan = 3;
  else
    fan = 0;  // auto (and anything unexpected)
  if (this->pending_climate_fan_.load() == -1)
    this->climate_fan_.store(fan);
  this->climate_ui_dirty_.store(true);
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

int PatioUI::screen_index_for_entity_(const std::string &entity_id) const {
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (this->screen_configured_[i] && this->screen_entity_[i] == entity_id)
      return i;
  }
  return -1;
}

// Cover position feedback (Lutron etc.): 0=closed/down, 100=open/up. Empty /
// "unknown" / "None" leave the sentinel -1 so the button-disable logic treats
// the cover as position-less (like Somfy RTS).
void PatioUI::on_screen_position_(std::string entity_id, std::string position) {
  int idx = this->screen_index_for_entity_(entity_id);
  if (idx < 0)
    return;
  if (position.empty() || position == "unknown" || position == "unavailable" || position == "None") {
    this->screen_pos_[idx].store(-1);
  } else {
    char *end = nullptr;
    long p = strtol(position.c_str(), &end, 10);
    if (end == position.c_str()) {
      this->screen_pos_[idx].store(-1);
    } else {
      if (p < 0)
        p = 0;
      if (p > 100)
        p = 100;
      this->screen_pos_[idx].store(static_cast<int>(p));
    }
  }
  this->screen_ui_dirty_.store(true);
  ESP_LOGD(TAG, "screen[%d] position %s", idx, position.c_str());
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

// ---- onboard BM8563 RTC (PCF8563-compatible) on the BSP's shared I2C bus ----
// Register map (BCD): 0x02 sec(+VL bit7) | 0x03 min | 0x04 hour | 0x05 day |
// 0x06 weekday | 0x07 month(+century bit7) | 0x08 year(00-99). The chip stores
// UTC. We share the BSP's single I2C master (via bsp_i2c_get_handle) so there
// is no second-master bus contention — a parallel ESPHome i2c bus on the same
// pins fails with "I2C software timeout"/bus-recovery, so it must go through the
// BSP handle exactly like the AXP2101 CHGLED write above.
static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t) ((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t) (((d / 10) << 4) | (d % 10)); }

// Days since 1970-01-01 for a UTC civil date (Howard Hinnant's algorithm) — lets
// us build a UTC epoch without depending on timegm()/timezone state.
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int64_t) doe - 719468;
}

void PatioUI::seed_clock_from_rtc_() {
  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (bus == nullptr)
    return;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = 0x51;  // BM8563
  cfg.scl_speed_hz = 100000;
  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
    ESP_LOGW(TAG, "RTC: add_device failed; clock will wait for HA");
    return;
  }
  this->rtc_dev_ = dev;  // kept for periodic write-back from HA time

  uint8_t reg = 0x02;
  uint8_t b[7] = {0};
  if (i2c_master_transmit_receive(dev, &reg, 1, b, sizeof(b), 1000) != ESP_OK) {
    ESP_LOGW(TAG, "RTC: read failed; clock will wait for HA");
    return;
  }
  bool vl = (b[0] & 0x80) != 0;  // voltage-low: time integrity not guaranteed
  int sec = bcd2dec(b[0] & 0x7F);
  int minute = bcd2dec(b[1] & 0x7F);
  int hour = bcd2dec(b[2] & 0x3F);
  int mday = bcd2dec(b[3] & 0x3F);
  int mon = bcd2dec(b[5] & 0x1F);       // 1..12
  int year = 2000 + bcd2dec(b[6]);      // century bit ignored (base 2000)
  if (vl || year < 2021 || mon < 1 || mon > 12 || mday < 1 || mday > 31) {
    ESP_LOGW(TAG, "RTC: time not trustworthy (VL=%d %04d-%02d-%02d); waiting for HA", vl, year, mon, mday);
    return;
  }
  int64_t epoch = days_from_civil(year, (unsigned) mon, (unsigned) mday) * 86400LL + hour * 3600 + minute * 60 + sec;
  if (epoch <= 0)
    return;
  struct timeval tv = {};
  tv.tv_sec = (time_t) epoch;
  settimeofday(&tv, nullptr);
  ESP_LOGI(TAG, "RTC: seeded system clock %04d-%02d-%02d %02d:%02d:%02d UTC", year, mon, mday, hour, minute, sec);
}

void PatioUI::write_rtc_from_system_() {
  if (this->rtc_dev_ == nullptr || this->time_ == nullptr)
    return;
  auto now = this->time_->now();
  if (!now.is_valid())
    return;
  time_t epoch = (time_t) now.timestamp;  // UTC
  struct tm t;
  gmtime_r(&epoch, &t);
  auto dev = (i2c_master_dev_handle_t) this->rtc_dev_;
  // Ensure the oscillator runs (Control1 STOP bit clear) before writing time.
  const uint8_t ctrl1_run[] = {0x00, 0x00};
  i2c_master_transmit(dev, ctrl1_run, sizeof(ctrl1_run), 1000);
  uint8_t buf[8];
  buf[0] = 0x02;  // auto-incrementing start register
  buf[1] = dec2bcd(t.tm_sec) & 0x7F;   // clears VL
  buf[2] = dec2bcd(t.tm_min) & 0x7F;
  buf[3] = dec2bcd(t.tm_hour) & 0x3F;
  buf[4] = dec2bcd(t.tm_mday) & 0x3F;
  buf[5] = (uint8_t) (t.tm_wday & 0x07);
  buf[6] = (dec2bcd(t.tm_mon + 1) & 0x1F) | 0x80;  // century bit -> 20xx
  buf[7] = dec2bcd((t.tm_year + 1900) - 2000);
  if (i2c_master_transmit(dev, buf, sizeof(buf), 1000) != ESP_OK)
    ESP_LOGW(TAG, "RTC: write-back failed");
  else
    ESP_LOGD(TAG, "RTC: persisted HA time to chip");
}

#ifdef PATIO_AUDIO
// On-demand audio hardware self-test for the CoreS3 (NOT called at boot -- the
// patio_ui `speaker`/`microphone` platforms now own the codecs). Retained as a
// standalone bring-up diagnostic: call manually to prove the path. Uses the BSP
// codec init (bsp_audio_codec_speaker_init -> AW88298,
// bsp_audio_codec_microphone_init -> ES7210), which internally brings up the
// shared duplex I2S bus, reuses the BSP's I2C bus, drives the AW9523 speaker/mic
// enable, and sets the amp gain. Pushes a short 440 Hz tone out the speaker and
// reads a slice of mic audio back, logging RMS + peak so a level that tracks the
// room proves the capture path. Blocks ~1 s.
void PatioUI::audio_selftest_() {
  esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
  esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
  if (spk == nullptr || mic == nullptr) {
    ESP_LOGE(TAG, "audio selftest: codec init failed (spk=%p mic=%p)", spk, mic);
    return;
  }

  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 1;
  fs.channel_mask = 0;
  fs.sample_rate = 16000;
  fs.mclk_multiple = 0;

  // --- Playback: ~0.5 s of 440 Hz sine, near full-scale (the onboard 1 W
  // speaker is quiet, so drive it hard). Buffer MUST come from PSRAM: internal
  // RAM is nearly exhausted by the LVGL draw buffers + IRAM, so a plain malloc
  // returns null here. ---
  const int spk_samples = 16000 / 2;
  int16_t *tone = static_cast<int16_t *>(heap_caps_malloc(spk_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (tone != nullptr) {
    for (int i = 0; i < spk_samples; i++)
      tone[i] = static_cast<int16_t>(28000.0f * sinf(2.0f * (float) M_PI * 440.0f * i / 16000.0f));
    esp_codec_dev_set_out_vol(spk, 100);  // no-op before open; must be set after
    if (esp_codec_dev_open(spk, &fs) == ESP_OK) {
      // The BSP configures the AW88298 with hw_gain.pa_gain=15, so the driver's
      // set_vol() subtracts 15 dB -> esp_codec_dev_set_out_vol(spk,100) only
      // reaches -15 dB digital. Force the volume register (0x0C) to true 0 dB
      // directly over the BSP's shared I2C bus (AW88298 @ 0x36). This is a
      // DIGITAL-only write (high byte = attenuation; 0x00 = 0 dB, low byte 0x64
      // as the driver uses) -- no analog/boost change, so no hardware risk. The
      // boost converter (REG61) is already enabled at 0x0673 by the driver;
      // 0x6673 is only the boost-off power-on default.
      // TODO(upstream): the BSP buries pa_gain=15 with no way to reach 0 dB via
      // the public API -- expose it or drop the offset.
      esp_codec_dev_set_out_vol(spk, 100);  // now is_open==true -> writes -15 dB
      i2c_master_bus_handle_t abus = bsp_i2c_get_handle();
      if (abus != nullptr) {
        i2c_device_config_t aw_cfg = {};
        aw_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        aw_cfg.device_address = 0x36;  // AW88298
        aw_cfg.scl_speed_hz = 100000;
        i2c_master_dev_handle_t aw = nullptr;
        if (i2c_master_bus_add_device(abus, &aw_cfg, &aw) == ESP_OK) {
          const uint8_t vol_0db[] = {0x0C, 0x00, 0x64};  // REG0C hi=0x00 -> 0 dB
          if (i2c_master_transmit(aw, vol_0db, sizeof(vol_0db), 1000) != ESP_OK)
            ESP_LOGW(TAG, "audio selftest: AW88298 vol write failed");
          i2c_master_bus_rm_device(aw);
        }
      }
      esp_codec_dev_dump_reg(spk);  // log actual REG0C/REG61 for ground truth
      esp_codec_dev_write(spk, tone, spk_samples * sizeof(int16_t));
      esp_codec_dev_close(spk);
    } else {
      ESP_LOGE(TAG, "audio selftest: speaker open failed");
    }
    free(tone);
  } else {
    ESP_LOGE(TAG, "audio selftest: tone alloc failed");
  }

  // --- Capture: ~0.5 s, report RMS + peak so a non-trivial level confirms the
  // ES7210 mics are live. ---
  const int mic_samples = 16000 / 2;
  int16_t *cap = static_cast<int16_t *>(heap_caps_malloc(mic_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (cap != nullptr) {
    esp_codec_dev_set_in_gain(mic, 30.0f);
    if (esp_codec_dev_open(mic, &fs) == ESP_OK) {
      if (esp_codec_dev_read(mic, cap, mic_samples * sizeof(int16_t)) == ESP_OK) {
        double sumsq = 0.0;
        int peak = 0;
        for (int i = 0; i < mic_samples; i++) {
          sumsq += static_cast<double>(cap[i]) * cap[i];
          int a = cap[i] < 0 ? -cap[i] : cap[i];
          if (a > peak)
            peak = a;
        }
        ESP_LOGI(TAG, "audio selftest: OK — tone played, mic RMS=%.1f peak=%d",
                 sqrt(sumsq / mic_samples), peak);
      } else {
        ESP_LOGE(TAG, "audio selftest: mic read failed");
      }
      esp_codec_dev_close(mic);
    } else {
      ESP_LOGE(TAG, "audio selftest: mic open failed");
    }
    free(cap);
  } else {
    ESP_LOGE(TAG, "audio selftest: capture alloc failed");
  }
}
#endif  // PATIO_AUDIO

void PatioUI::setup() {
  ESP_LOGI(TAG, "bringing up display + LVGL");


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
#ifdef PATIO_DISPLAY_PSRAM
  // CoreS3 (ESP32-S3, SOC_PSRAM_DMA_CAPABLE == 1): allocate the LVGL draw
  // buffers in PSRAM instead of scarce internal DMA SRAM. The default
  // bsp_display_start() hardcodes buff_dma=true / buff_spiram=false, pinning the
  // ~51 KB (40-row double) draw buffer in internal RAM — which leaves no room
  // for esp-sr's AEC (~31 KB internal floor it cannot take from PSRAM, causing
  // the "LVGL buffer (buf2) allocation" boot-loop). The S3's SPI DMA can read
  // the flush buffer straight from PSRAM, so moving it there frees the internal
  // RAM the AEC needs. buff_dma stays TRUE alongside buff_spiram: the S3's PSRAM
  // is DMA-capable, so the port must allocate with MALLOC_CAP_DMA|MALLOC_CAP_SPIRAM
  // for the SPI panel to DMA the flush straight from PSRAM. (With buff_dma=false
  // the buffer is not DMA-registered and the flush wedges — the screen freezes.)
  bsp_display_cfg_t disp_cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
      .double_buffer = true,
      .flags = {
          .buff_dma = true,
          .buff_spiram = true,
          .sw_rotate = false,
      },
  };
  lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
#else
  lv_display_t *disp = bsp_display_start();
#endif
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
        // Enable the battery-voltage ADC (ADC channel ctrl reg 0x30, bit0) so
        // the voltage read used as a battery-percentage fallback is valid.
        uint8_t reg30 = 0x30, adcen = 0;
        if (i2c_master_transmit_receive(axp, &reg30, 1, &adcen, 1, 1000) == ESP_OK) {
          const uint8_t w[] = {0x30, (uint8_t) (adcen | 0x01)};
          i2c_master_transmit(axp, w, sizeof(w), 1000);
        }
        this->axp_dev_ = axp;  // kept for periodic battery/power polling in loop()
      }
    }
  }

  // Seed the system clock from the battery-backed BM8563 RTC so the time tile
  // (and the persisted-timer restore below) has a valid wall clock immediately,
  // instead of waiting ~seconds for WiFi + the Home Assistant time sync.
  this->seed_clock_from_rtc_();

  bsp_display_lock(0);
  this->build_ui_();
  bsp_display_unlock();

  // Fling-smoothness fix: the esp_lvgl_port task over-sleeps between frames (its
  // loop sleeps up to task_max_sleep_ms=500 ms whenever lv_timer_handler()
  // reports no imminent timer), so an active scroll animation's elapsed-time
  // calc jumps it to completion in 1-2 giant steps instead of stepping smoothly.
  // A persistent high-frequency no-op timer forces lv_timer_handler() to always
  // report work due within 5 ms, so the port task can never fall into the 500 ms
  // sleep. Confirmed on hardware to be the root-cause fix for choppy flings —
  // this is load-bearing, do not remove.
  lv_timer_create([](lv_timer_t *) {}, 5, nullptr);

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
  if (this->active_.load() && this->tv_ != nullptr && this->heater_tile_ != nullptr) {
    bsp_display_lock(0);
    lv_tileview_set_tile(this->tv_, this->heater_tile_, LV_ANIM_OFF);
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

  // Screens: Somfy RTS is command-only (no feedback), but position-reporting
  // covers (Lutron etc.) publish current_position — subscribe so we can disable
  // up/down at the travel limits and draw a position-fill indicator. Covers
  // that never report position keep the sentinel -1 and are never disabled.
  for (int i = 0; i < NUM_SCREENS; i++) {
    this->screen_pos_[i].store(-1);
    if (this->screen_configured_[i])
      this->subscribe_homeassistant_state(&PatioUI::on_screen_position_, this->screen_entity_[i],
                                          "current_position");
  }
  this->screen_ui_dirty_.store(true);

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

  // Climate/thermostat: subscribe to the hvac mode (state) plus the attributes
  // we render, and init the pending-intent sentinels.
  this->pending_climate_target_x10_.store(-10000);
  this->pending_climate_mode_.store(-1);
  this->pending_climate_fan_.store(-1);
  if (this->climate_configured_) {
    this->subscribe_homeassistant_state(&PatioUI::on_climate_state_, this->climate_entity_);
    this->subscribe_homeassistant_state(&PatioUI::on_climate_action_, this->climate_entity_, "hvac_action");
    this->subscribe_homeassistant_state(&PatioUI::on_climate_cur_temp_, this->climate_entity_,
                                        "current_temperature");
    this->subscribe_homeassistant_state(&PatioUI::on_climate_target_temp_, this->climate_entity_, "temperature");
    this->subscribe_homeassistant_state(&PatioUI::on_climate_fan_, this->climate_entity_, "fan_mode");
  }
  this->climate_ui_dirty_.store(true);

  ESP_LOGI(TAG, "UI up; heater tile bound to %s", this->timer_entity_.c_str());

  // Live screen capture endpoint (uncompressed PNG on :8080/screenshot).
  this->start_screenshot_server_();
}

void PatioUI::loop() {
  // Keep the battery-backed RTC in sync with HA time: write once shortly after
  // boot (also clears the STOP/VL flags so it ticks) and every ~5 min after.
  if (this->rtc_dev_ != nullptr && this->time_ != nullptr && this->time_->now().is_valid()) {
    uint32_t nowms = millis();
    if (this->last_rtc_write_ms_ == 0 || (nowms - this->last_rtc_write_ms_) >= 300000UL) {
      this->write_rtc_from_system_();
      this->last_rtc_write_ms_ = nowms;
    }
  }

  // Poll battery/power (AXP2101) and network state on a light cadence; the
  // clock-tile status icons read the resulting atomics on the LVGL task.
  {
    uint32_t nowms = millis();
    if (this->last_status_poll_ms_ == 0 || (nowms - this->last_status_poll_ms_) >= 3000UL) {
      this->poll_power_status_();
      this->last_status_poll_ms_ = nowms;
    }
  }

  // On battery, drop into deep sleep once the screen has been idle-asleep long
  // enough (never on USB). Runs on the main task with a fresh on_battery_.
  this->maybe_deep_sleep_();

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

  // Drain pending climate intents against the thermostat.
  if (this->climate_configured_) {
    int ct = this->pending_climate_target_x10_.exchange(-10000);
    if (ct != -10000) {
      float send = ct / 10.0f;
      if (this->climate_celsius_)  // stored/displayed °C -> HA wants °F
        send = send * 9.0f / 5.0f + 32.0f;
      char t[16];
      snprintf(t, sizeof(t), "%.1f", send);
      ESP_LOGI(TAG, "climate setpoint %s -> %s", t, this->climate_entity_.c_str());
      this->call_homeassistant_service("climate.set_temperature",
                                       {{"entity_id", this->climate_entity_}, {"temperature", t}});
    }
    int cm = this->pending_climate_mode_.exchange(-1);
    if (cm >= 0 && cm < CLM_COUNT) {
      ESP_LOGI(TAG, "climate mode %s -> %s", CLIMATE_MODE_HA[cm], this->climate_entity_.c_str());
      this->call_homeassistant_service(
          "climate.set_hvac_mode", {{"entity_id", this->climate_entity_}, {"hvac_mode", CLIMATE_MODE_HA[cm]}});
    }
    int cf = this->pending_climate_fan_.exchange(-1);
    if (cf >= 0 && cf < CLIMATE_FAN_N) {
      ESP_LOGI(TAG, "climate fan %s -> %s", CLIMATE_FAN_HA[cf], this->climate_entity_.c_str());
      this->call_homeassistant_service(
          "climate.set_fan_mode", {{"entity_id", this->climate_entity_}, {"fan_mode", CLIMATE_FAN_HA[cf]}});
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
  if (this->climate_configured_)
    ESP_LOGCONFIG(TAG, "  climate   %-9s -> %s (%.0f..%.0f step %.1f)", this->climate_label_.c_str(),
                  this->climate_entity_.c_str(), this->climate_min_, this->climate_max_, this->climate_step_);
}

}  // namespace patio_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
