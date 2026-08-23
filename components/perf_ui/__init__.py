import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID

CODEOWNERS = ["@sslivins"]
DEPENDENCIES = ["esp32"]

perf_ui_ns = cg.esphome_ns.namespace("perf_ui")
PerfUI = perf_ui_ns.class_("PerfUI", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PerfUI),
    }
).extend(cv.COMPONENT_SCHEMA)

# Same esp-bsp fork commit as patio_ui so the CoreS3 display/touch/LVGL stack is
# byte-for-byte identical to the full app — the whole point is to compare like
# for like.
ESP_BSP_REPO = "https://github.com/sslivins/esp-bsp.git"
ESP_BSP_REF = "1aece77420cf6d89e16f17b42eb67ef06c099075"


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Pull the CoreS3 BSP (esp_lcd SPI panel + FT6336 touch + esp_lvgl_port DMA
    # flush). It transitively pulls esp_lvgl_port + lvgl, exactly as in patio_ui.
    esp32.add_idf_component(
        name="m5stack_core_s3",
        repo=ESP_BSP_REPO,
        ref=ESP_BSP_REF,
        path="bsp/m5stack_core_s3",
    )

    # --- board / performance (mirrors patio_ui) ---
    esp32.add_idf_sdkconfig_option("CONFIG_BSP_PMU_AXP2101", True)
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240", True)
    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_HZ", 1000)

    # --- LVGL (v9, via esp_lvgl_port) ---
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_TILEVIEW", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_48", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_MEM_SIZE_KILOBYTES", 64)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_PERF_MONITOR", True)

    # PSRAM full-frame draw buffers must be aligned to the data-cache line (64 B)
    # or the SPI GDMA cannot transmit directly from PSRAM and falls back to an
    # internal "priv TX" bounce buffer sized to the whole transfer — which for a
    # 150 KB full frame fails to allocate and crash-loops the LVGL task. The IDF
    # default LV_DRAW_BUF_ALIGN is 4; force 64 so PSRAM-source DMA works.
    esp32.add_idf_sdkconfig_option("CONFIG_LV_DRAW_BUF_ALIGN", 64)

    # --- CoreS3 render-performance flags: compiler -O2 + LVGL hot paths in
    # IRAM. LVGL's software renderer is pixel-loop heavy and the IDF default is
    # -Os (size); -O2 typically 2-3x's these loops.
    esp32.add_idf_sdkconfig_option("CONFIG_COMPILER_OPTIMIZATION_SIZE", False)
    esp32.add_idf_sdkconfig_option("CONFIG_COMPILER_OPTIMIZATION_PERF", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM", True)
