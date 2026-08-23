#include "perf_ui.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"

namespace esphome {
namespace perf_ui {

static const char *const TAG = "perf_ui";

// Maximum time the esp_lvgl_port task is allowed to sleep between iterations
// when LVGL reports no timer is imminently due. The BSP default is 500 ms.
// Cadence hypothesis: during a scroll animation the task over-sleeps up to this
// long, so the animation's elapsed-time math jumps it to completion in 1-2
// giant steps instead of stepping smoothly. Lower this to force a fast loop.
#ifndef PERF_TASK_MAX_SLEEP_MS
#define PERF_TASK_MAX_SLEEP_MS 500
#endif

// --- Buffer knobs (edit + recompile = ~1-2 min, no full rebuild) ---
// PERF_BUFF_SPIRAM: 1 = draw buffer in PSRAM (big, slow to rasterize into).
//                   0 = draw buffer in internal DMA SRAM (small, fast render).
// PERF_BUFF_ROWS:   height in rows of each draw buffer (screen is 240 tall).
// PERF_DOUBLE_BUFFER: 1 = two buffers (lets render overlap flush).
#ifndef PERF_BUFF_SPIRAM
#define PERF_BUFF_SPIRAM 0
#endif
#ifndef PERF_BUFF_ROWS
#define PERF_BUFF_ROWS 40
#endif
#ifndef PERF_DOUBLE_BUFFER
#define PERF_DOUBLE_BUFFER 1
#endif
// PERF_DRAW_LABEL: 1 = big montserrat-48 label per tile; 0 = solid tiles only
// (isolates font-glyph rasterization cost from fill/composite cost).
#ifndef PERF_DRAW_LABEL
#define PERF_DRAW_LABEL 0
#endif
// PERF_HORIZONTAL: 1 = horizontal tile fling (LV_DIR_HOR) to match the real
// patio hub; 0 = vertical (LV_DIR_TOP|BOTTOM). Direction changes the buffer
// memory-access pattern, so measure the one that matters (horizontal).
#ifndef PERF_HORIZONTAL
#define PERF_HORIZONTAL 1
#endif
// PERF_FULLFRAME: 1 = two FULL-screen (240-row) buffers in PSRAM, double-
// buffered. A complete frame is always flushed at once => no mid-scan tear.
// Overrides PERF_BUFF_SPIRAM/ROWS/DOUBLE below. This is the tear-free target
// architecture; render speed (via -O2) determines whether it's also smooth.
#ifndef PERF_FULLFRAME
#define PERF_FULLFRAME 0
#endif
#if PERF_FULLFRAME
#undef PERF_BUFF_SPIRAM
#undef PERF_BUFF_ROWS
#undef PERF_DOUBLE_BUFFER
#define PERF_BUFF_SPIRAM 1
#define PERF_BUFF_ROWS 240
#define PERF_DOUBLE_BUFFER 1
#endif

void PerfUI::setup() {
  ESP_LOGI(TAG, "perf_ui: bringing up CoreS3 display + LVGL (isolated fling test), "
                "task_max_sleep_ms=%d spiram=%d rows=%d double=%d horiz=%d", PERF_TASK_MAX_SLEEP_MS,
           PERF_BUFF_SPIRAM, PERF_BUFF_ROWS, PERF_DOUBLE_BUFFER, PERF_HORIZONTAL);

  // Bring up the display exactly like the BSP's bsp_display_start(), but with
  // the draw buffer in PSRAM instead of internal DMA RAM. The CoreS3 (ESP32-S3)
  // has DMA-capable PSRAM, so a PSRAM buffer flushes fine; the default 100-row
  // double buffer (128 KB) does not fit in internal DMA RAM once WiFi + the
  // 64 KB LVGL heap are reserved, so the stock bsp_display_start() OOM-asserts.
  bsp_display_cfg_t cfg = {};
  cfg.lvgl_port_cfg.task_priority = 4;
  cfg.lvgl_port_cfg.task_stack = 7168;
  cfg.lvgl_port_cfg.task_affinity = -1;
  cfg.lvgl_port_cfg.task_max_sleep_ms = PERF_TASK_MAX_SLEEP_MS;
  cfg.lvgl_port_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
  cfg.lvgl_port_cfg.timer_period_ms = 5;
  cfg.buffer_size = BSP_LCD_H_RES * PERF_BUFF_ROWS;
  cfg.double_buffer = PERF_DOUBLE_BUFFER;
  // On the ESP32-S3 a full-frame PSRAM canvas must be allocated DMA-capable
  // (MALLOC_CAP_DMA|MALLOC_CAP_SPIRAM) so the SPI GDMA can transmit directly
  // from PSRAM. Setting only buff_spiram yields a non-DMA PSRAM buffer that the
  // SPI master would have to bounce. For full-frame we therefore request both.
  cfg.flags.buff_dma = PERF_FULLFRAME ? true : !PERF_BUFF_SPIRAM;
  cfg.flags.buff_spiram = PERF_BUFF_SPIRAM;
  cfg.flags.sw_rotate = false;
  lv_display_t *disp = bsp_display_start_with_config(&cfg);
  if (disp == nullptr) {
    ESP_LOGE(TAG, "bsp_display_start failed");
    this->mark_failed();
    return;
  }
  bsp_display_backlight_on();
  bsp_display_brightness_set(80);

  // --- swipe-perf diagnostic (identical to patio_ui's) ---
  // RENDER_START..RENDER_READY = CPU render time; ..REFR_READY adds SPI flush.
  // Log FPS + busy-% once per second while anything animates.
  static int64_t perf_render_start_us = 0;
  static int64_t perf_render_us_sum = 0;
  static int64_t perf_total_us_sum = 0;
  static int perf_frames = 0;
  static bool perf_rendered = false;
  static int64_t perf_window_start_us = 0;
  lv_display_add_event_cb(disp, [](lv_event_t *) { perf_render_start_us = esp_timer_get_time(); },
                          LV_EVENT_RENDER_START, nullptr);
  lv_display_add_event_cb(disp, [](lv_event_t *) {
    perf_render_us_sum += esp_timer_get_time() - perf_render_start_us;
    perf_rendered = true;
  }, LV_EVENT_RENDER_READY, nullptr);
  lv_display_add_event_cb(disp, [](lv_event_t *) {
    if (!perf_rendered)
      return;
    int64_t now = esp_timer_get_time();
    perf_total_us_sum += now - perf_render_start_us;
    perf_frames++;
    perf_rendered = false;
    if (perf_window_start_us == 0)
      perf_window_start_us = now;
    if (now - perf_window_start_us >= 1000000) {
      int64_t window_us = now - perf_window_start_us;
      int busy_pct = (int) (window_us ? (perf_render_us_sum * 100) / window_us : 0);
      ESP_LOGI(TAG, "swipe-perf: fps=%d render_avg=%lldus total_avg=%lldus busy=%d%% cpu=%dMHz",
               perf_frames, (long long) (perf_frames ? perf_render_us_sum / perf_frames : 0),
               (long long) (perf_frames ? perf_total_us_sum / perf_frames : 0), busy_pct,
               esp_clk_cpu_freq() / 1000000);
      perf_frames = 0;
      perf_render_us_sum = 0;
      perf_total_us_sum = 0;
      perf_window_start_us = now;
    }
  }, LV_EVENT_REFR_READY, nullptr);

  // --- bare tileview: a few trivial tiles (big label on a solid background) ---
  static const uint32_t bg_cols[6] = {0x102840, 0x104028, 0x402810,
                                      0x401028, 0x284010, 0x281040};
  static const char *const names[6] = {"TILE 1", "TILE 2", "TILE 3",
                                       "TILE 4", "TILE 5", "TILE 6"};
  bsp_display_lock(0);
  lv_obj_t *scr = lv_screen_active();
  this->tv_ = lv_tileview_create(scr);
  lv_obj_set_size(this->tv_, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(this->tv_, lv_color_hex(0x000000), 0);
  for (int i = 0; i < 6; i++) {
#if PERF_HORIZONTAL
    lv_obj_t *t = lv_tileview_add_tile(this->tv_, (uint8_t) i, 0, LV_DIR_HOR);
#else
    lv_obj_t *t = lv_tileview_add_tile(this->tv_, 0, (uint8_t) i, (lv_dir_t) (LV_DIR_TOP | LV_DIR_BOTTOM));
#endif
    lv_obj_set_style_bg_color(t, lv_color_hex(bg_cols[i]), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
#if PERF_DRAW_LABEL
    lv_obj_t *lbl = lv_label_create(t);
    lv_label_set_text(lbl, names[i]);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);
    lv_obj_center(lbl);
#endif
    this->tiles_[i] = t;
  }
  this->num_tiles_ = 6;
  bsp_display_unlock();

  // --- self-driving CONTINUOUS fling: retarget to the next tile every 120 ms ---
  // The tileview scroll animation lasts a few hundred ms; by retargeting faster
  // than it completes we keep the display animating NON-STOP, so the once-per-
  // second fps metric reflects the TRUE in-motion frame rate instead of being
  // diluted by idle gaps between discrete flings.
  lv_timer_create(
      [](lv_timer_t *t) {
        PerfUI *self = static_cast<PerfUI *>(lv_timer_get_user_data(t));
        if (self->tv_ == nullptr || self->num_tiles_ < 2)
          return;
        static int idx = 0;
        idx = (idx + 1) % self->num_tiles_;
        lv_tileview_set_tile(self->tv_, self->tiles_[idx], LV_ANIM_ON);
        lv_display_trigger_activity(nullptr);
      },
      120, this);
}

}  // namespace perf_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
