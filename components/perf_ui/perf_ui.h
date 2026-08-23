#pragma once

#ifdef USE_ESP_IDF

#include "esphome/core/component.h"
#include "lvgl.h"

namespace esphome {
namespace perf_ui {

// Minimal, self-contained LVGL fling-performance harness for the M5Stack
// CoreS3. It brings up the exact same esp-bsp + esp_lvgl_port + LVGL stack the
// patio_ui component uses, then builds a bare tileview of a few trivial tiles
// and self-drives a scroll animation through them while logging frame timing.
// Nothing else runs (no Home Assistant, no CPU scaling, no screen-sleep), so if
// the fling is choppy here the problem is in the BSP/LVGL/display baseline, not
// in patio_ui.
class PerfUI : public Component {
 public:
  void setup() override;
  void loop() override {}
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  lv_obj_t *tv_{nullptr};
  lv_obj_t *tiles_[8]{};
  int num_tiles_{0};
};

}  // namespace perf_ui
}  // namespace esphome

#endif  // USE_ESP_IDF
