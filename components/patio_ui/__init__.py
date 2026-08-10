import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, time as time_
from esphome.const import CONF_ID, CONF_TIME_ID

CODEOWNERS = ["@sslivins"]
DEPENDENCIES = ["esp32", "api", "time"]

CONF_RUN_SCRIPT = "run_script"
CONF_STOP_SCRIPT = "stop_script"
CONF_TIMER_ENTITY = "timer_entity"
CONF_TEMP_SENSOR = "temp_sensor"
CONF_DEFAULT_MINUTES = "default_minutes"
CONF_MIN_MINUTES = "min_minutes"
CONF_MAX_MINUTES = "max_minutes"

CONF_SCREENS = "screens"
CONF_LIGHTS = "lights"
CONF_ENTITY_ID = "entity_id"
CONF_LABEL = "label"

# Fixed on-screen slots for the deck-perimeter map, as seen by someone standing
# in front of the Core2 (on the north wall). "rear_*" are the two side-by-side
# screens on the wall behind the viewer. Order here is the slot index passed to
# the C++ component.
SCREEN_SLOTS = ["left", "right", "rear_left", "rear_right"]
SCREEN_DEFAULT_LABELS = {
    "left": "Left",
    "right": "Right",
    "rear_left": "Rear Left",
    "rear_right": "Rear Right",
}

# Two dimmable light groups on the patio, shown as side-by-side vertical faders.
# Order here is the slot index passed to the C++ component.
LIGHT_SLOTS = ["main", "bbq"]
LIGHT_DEFAULT_LABELS = {
    "main": "Main",
    "bbq": "BBQ",
}

# Pinned to our esp-bsp fork branch (PR espressif/esp-bsp#813). This BSP:
#   * uses the new i2c-ng master driver, so FT6336 touch init works under
#     IDF >=5.5 with no scl_speed_hz patch, and
#   * includes our Core2 v1.1 AXP2101 ALDO2 LCD/touch reset-pulse fix, so the
#     panel comes up (no blank screen) with the BSP doing power + reset itself.
# Only the bsp/m5stack_core_2 folder is pulled; its registry dependencies
# (esp_lvgl_port -> LVGL 9, esp_lcd_ili9341, esp_lcd_touch_ft5x06) are resolved
# by the IDF component manager at build time.
ESP_BSP_REPO = "https://github.com/sslivins/esp-bsp.git"
ESP_BSP_REF = "696ef8657e2b48f849e83c3726c2e69cad18b41d"
ESP_BSP_PATH = "bsp/m5stack_core_2"

patio_ui_ns = cg.esphome_ns.namespace("patio_ui")
PatioUI = patio_ui_ns.class_("PatioUI", cg.Component)

SCREEN_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.string,
        cv.Optional(CONF_LABEL): cv.string,
    }
)

LIGHT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.string,
        cv.Optional(CONF_LABEL): cv.string,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PatioUI),
        cv.Optional(CONF_RUN_SCRIPT, default="script.patio_heater_run"): cv.string,
        cv.Optional(CONF_STOP_SCRIPT, default="script.patio_heater_stop"): cv.string,
        cv.Optional(CONF_TIMER_ENTITY, default="timer.patio_heaters"): cv.string,
        cv.Optional(
            CONF_TEMP_SENSOR, default="sensor.usl_environmental_temperature_3"
        ): cv.string,
        cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_DEFAULT_MINUTES, default=30): cv.int_range(min=1, max=1440),
        cv.Optional(CONF_MIN_MINUTES, default=5): cv.int_range(min=1, max=1440),
        cv.Optional(CONF_MAX_MINUTES, default=480): cv.int_range(min=1, max=1440),
        cv.Optional(CONF_SCREENS): cv.Schema(
            {cv.Optional(slot): SCREEN_SCHEMA for slot in SCREEN_SLOTS}
        ),
        cv.Optional(CONF_LIGHTS): cv.Schema(
            {cv.Optional(slot): LIGHT_SCHEMA for slot in LIGHT_SLOTS}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_run_script(config[CONF_RUN_SCRIPT]))
    cg.add(var.set_stop_script(config[CONF_STOP_SCRIPT]))
    cg.add(var.set_timer_entity(config[CONF_TIMER_ENTITY]))
    cg.add(var.set_temp_sensor(config[CONF_TEMP_SENSOR]))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_default_minutes(config[CONF_DEFAULT_MINUTES]))
    cg.add(var.set_min_minutes(config[CONF_MIN_MINUTES]))
    cg.add(var.set_max_minutes(config[CONF_MAX_MINUTES]))

    # Perimeter screen controls (each slot -> fixed on-screen position).
    screens = config.get(CONF_SCREENS, {})
    for idx, slot in enumerate(SCREEN_SLOTS):
        if slot in screens:
            sc = screens[slot]
            label = sc.get(CONF_LABEL, SCREEN_DEFAULT_LABELS[slot])
            cg.add(var.add_screen(idx, sc[CONF_ENTITY_ID], label))

    # Dimmable light groups (each slot -> a vertical fader).
    lights = config.get(CONF_LIGHTS, {})
    for idx, slot in enumerate(LIGHT_SLOTS):
        if slot in lights:
            lt = lights[slot]
            label = lt.get(CONF_LABEL, LIGHT_DEFAULT_LABELS[slot])
            cg.add(var.add_light(idx, lt[CONF_ENTITY_ID], label))

    # Pull the M5Stack Core2 BSP (esp_lcd + esp_lvgl_port DMA flush + touch).
    esp32.add_idf_component(
        name="m5stack_core_2",
        repo=ESP_BSP_REPO,
        ref=ESP_BSP_REF,
        path=ESP_BSP_PATH,
    )

    # --- board / performance ---
    esp32.add_idf_sdkconfig_option("CONFIG_BSP_PMU_AXP2101", True)  # Core2 v1.1 PMU
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240", True)
    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_HZ", 1000)

    # --- LVGL (v9, pulled in via esp_lvgl_port) ---
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_TILEVIEW", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_ARC", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_ROLLER", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_SNAPSHOT", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_20", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_28", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_48", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_MEM_SIZE_KILOBYTES", 64)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_PERF_MONITOR", True)
