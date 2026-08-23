import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
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
CONF_MEDIA = "media"
CONF_CLIMATE = "climate"
CONF_ENTITY_ID = "entity_id"
CONF_LABEL = "label"

CONF_TILES = "tiles"
CONF_MIN_TEMP = "min_temp"
CONF_MAX_TEMP = "max_temp"
CONF_TEMP_STEP = "temp_step"

CONF_TITLE = "title"
CONF_LAYOUT = "layout"
CONF_UNIT = "unit"

# Swipeable tile kinds. The YAML `tiles:` list selects which tiles are built and
# in what order; these ints MUST match the TileKind enum in patio_ui.h.
TILE_KINDS = {
    "time": 0,
    "heater": 1,
    "climate": 2,
    "lights": 3,
    "screens": 4,
    "media": 5,
}
# Default tile set = the original patio layout, so existing configs that omit
# `tiles:` behave exactly as before.
DEFAULT_TILES = ["time", "heater", "lights", "screens", "media"]

# Automation fired when the component decides to enter deep sleep (on battery +
# screen idle). The board entrypoint wires this to the `deep_sleep` component.
CONF_ON_DEEP_SLEEP_REQUEST = "on_deep_sleep_request"

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
# Only the selected board's bsp/<board> folder is pulled; its registry
# dependencies (esp_lvgl_port -> LVGL 9, esp_lcd_* , esp_lcd_touch_*) are
# resolved by the IDF component manager at build time.
#
# The C++ component only ever talks to the generic esp-bsp API (bsp_display_*,
# bsp_i2c_get_handle) via <bsp/esp-bsp.h>, so switching boards is purely a
# matter of pulling a different bsp/<board> component + a couple of board
# sdkconfig options — no source changes.
ESP_BSP_REPO = "https://github.com/sslivins/esp-bsp.git"
# PR espressif/esp-bsp#816 head (stacked on #813): Core2 v1.1 AXP2101 fixes.
#  - #813: failure-safe read8bit_checked + ALDO2 LCD/touch reset (20 ms assert /
#    10 ms release). Validated on hardware 2026-08-11 (10/10 cold boots, incl.
#    battery-pull, panel up every time).
#  - #816: bsp_display_brightness_set owns the BLDO1 backlight rail so 0% fully
#    cuts the rail (true-off, no ~2.5 V faint glow) and >0% re-enables it.
ESP_BSP_REF = "1aece77420cf6d89e16f17b42eb67ef06c099075"

# Supported M5Stack boards. `variant` (default core2) selects which esp-bsp
# board component gets pulled and any board-specific sdkconfig. Both boards use
# the same 320x240 panel + AXP2101 PMU, so the whole UI layer is identical.
BSP_VARIANTS = {
    "core2": {
        "component": "m5stack_core_2",
        "path": "bsp/m5stack_core_2",
        "repo": ESP_BSP_REPO,
        "ref": ESP_BSP_REF,
        "axp2101": True,   # Core2 v1.1 PMU
    },
    "cores3": {
        "component": "m5stack_core_s3",
        "path": "bsp/m5stack_core_s3",
        "repo": ESP_BSP_REPO,
        # Stock BSP. A previous fork (perf/cores3-60mhz-spi) tried to raise the
        # LCD SPI pixel clock 40->60MHz, but on-device verification proved the
        # ESP32-S3 GPSPI divider only resolves to 40MHz (requests 41-60) or
        # 80MHz (requests 61-80) from its fixed 80MHz APB source -- a 60MHz
        # request runs at a real 40MHz, so the fork was a no-op. 80MHz produces
        # visible tearing/artifacts on this panel, so 40MHz stock is the right
        # value. Real fling smoothness comes from the sdkconfig perf flags below
        # (LV_DEF_REFR_PERIOD, -O2/IRAM, 40-row internal draw buffer), not SPI.
        "ref": ESP_BSP_REF,
        "axp2101": True,   # CoreS3 also uses an AXP2101 (+ an AW9523 expander)
    },
}
CONF_VARIANT = "variant"

patio_ui_ns = cg.esphome_ns.namespace("patio_ui")
PatioUI = patio_ui_ns.class_("PatioUI", cg.Component)

SCREEN_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.string,
        cv.Optional(CONF_LABEL): cv.string,
    }
)

# The full screens block: one entry per fixed slot, plus an optional tile title
# and layout. `perimeter` (default) is the patio deck map; `row` lays the covers
# out left-to-right with the rear/centre slots double-width (bedroom blinds).
SCREENS_SCHEMA = cv.Schema(
    {cv.Optional(slot): SCREEN_SCHEMA for slot in SCREEN_SLOTS}
).extend(
    {
        cv.Optional(CONF_TITLE, default="Screens"): cv.string,
        cv.Optional(CONF_LAYOUT, default="perimeter"): cv.one_of(
            "perimeter", "row", lower=True
        ),
    }
)

LIGHT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.string,
        cv.Optional(CONF_LABEL): cv.string,
    }
)

# Single deck media_player (Sonos amp): volume + transport control.
MEDIA_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.string,
        cv.Optional(CONF_LABEL, default="Music"): cv.string,
    }
)

# Single HA climate entity (thermostat/mini-split): setpoint + mode + fan. The
# setpoint range/step default to a comfortable whole-degree bedroom range.
CLIMATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.string,
        cv.Optional(CONF_LABEL, default="Climate"): cv.string,
        cv.Optional(CONF_MIN_TEMP, default=61.0): cv.float_,
        cv.Optional(CONF_MAX_TEMP, default=88.0): cv.float_,
        cv.Optional(CONF_TEMP_STEP, default=1.0): cv.positive_float,
        # Display/step unit. HA speaks whatever its system unit is (°F here);
        # "celsius" makes the tile show/step in °C and convert to/from °F.
        # When "celsius", express min/max/step below in °C.
        cv.Optional(CONF_UNIT, default="fahrenheit"): cv.one_of(
            "fahrenheit", "celsius", lower=True
        ),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PatioUI),
        cv.Optional(CONF_VARIANT, default="core2"): cv.one_of(
            *BSP_VARIANTS, lower=True
        ),
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
        cv.Optional(CONF_SCREENS): SCREENS_SCHEMA,
        cv.Optional(CONF_LIGHTS): cv.Schema(
            {cv.Optional(slot): LIGHT_SCHEMA for slot in LIGHT_SLOTS}
        ),
        cv.Optional(CONF_MEDIA): MEDIA_SCHEMA,
        cv.Optional(CONF_CLIMATE): CLIMATE_SCHEMA,
        cv.Optional(CONF_TILES, default=DEFAULT_TILES): cv.ensure_list(
            cv.one_of(*TILE_KINDS, lower=True)
        ),
        cv.Optional(CONF_ON_DEEP_SLEEP_REQUEST): automation.validate_automation(
            single=True
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
    cg.add(var.set_screen_title(screens.get(CONF_TITLE, "Screens")))
    cg.add(var.set_screen_layout(1 if screens.get(CONF_LAYOUT) == "row" else 0))
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

    # Deck media player (single Sonos amp: volume + transport).
    if CONF_MEDIA in config:
        media = config[CONF_MEDIA]
        cg.add(var.set_media_entity(media[CONF_ENTITY_ID]))
        cg.add(var.set_media_label(media[CONF_LABEL]))

    # Thermostat/climate (single HA climate entity: setpoint + mode + fan).
    if CONF_CLIMATE in config:
        clim = config[CONF_CLIMATE]
        cg.add(var.set_climate_entity(clim[CONF_ENTITY_ID]))
        cg.add(var.set_climate_label(clim[CONF_LABEL]))
        cg.add(var.set_climate_min(clim[CONF_MIN_TEMP]))
        cg.add(var.set_climate_max(clim[CONF_MAX_TEMP]))
        cg.add(var.set_climate_step(clim[CONF_TEMP_STEP]))
        cg.add(var.set_climate_celsius(clim[CONF_UNIT] == "celsius"))

    # Ordered tile set. add_tile() appends each kind in the configured order;
    # build_ui_ then builds only these tiles.
    for kind in config[CONF_TILES]:
        cg.add(var.add_tile(TILE_KINDS[kind]))

    # Deep-sleep hand-off: the component owns the policy (on battery + screen
    # idle) and fires this trigger; the board entrypoint attaches the mechanism
    # (deep_sleep.enter with GPIO39 tap-wake).
    if CONF_ON_DEEP_SLEEP_REQUEST in config:
        await automation.build_automation(
            var.get_deep_sleep_trigger(), [], config[CONF_ON_DEEP_SLEEP_REQUEST]
        )

    # Pull the selected M5Stack board's BSP (esp_lcd + esp_lvgl_port DMA flush
    # + touch). The C++ only uses the generic <bsp/esp-bsp.h> API, so the board
    # is chosen entirely here via `variant`.
    bsp = BSP_VARIANTS[config[CONF_VARIANT]]
    esp32.add_idf_component(
        name=bsp["component"],
        repo=bsp["repo"],
        ref=bsp["ref"],
        path=bsp["path"],
    )

    # --- board / performance ---
    if bsp["axp2101"]:
        esp32.add_idf_sdkconfig_option("CONFIG_BSP_PMU_AXP2101", True)  # AXP2101 PMU
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240", True)
    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_HZ", 1000)

    # --- LVGL (v9, pulled in via esp_lvgl_port) ---
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_TILEVIEW", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_ARC", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_ROLLER", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_DROPDOWN", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_SNAPSHOT", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_20", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_28", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_FONT_MONTSERRAT_48", True)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_MEM_SIZE_KILOBYTES", 64)
    esp32.add_idf_sdkconfig_option("CONFIG_LV_USE_PERF_MONITOR", True)

    # LVGL / LCD render performance (per Espressif's esp_lvgl_port performance
    # guide). Tile fling/scroll animations are render-CPU-bound on the Xtensa S3
    # (no LVGL blend assembly exists for Xtensa — only ARM NEON/Helium and the
    # RISC-V path that makes the ESP32-P4 Tab5 smooth). These flags are the
    # canonical, benchmark-backed fixes and roughly doubled FPS in Espressif's
    # tests. Our frame buffer (100-row double buffer in internal DMA SRAM) is
    # already in the recommended >25%-of-screen zone, so the wins here come from
    # CPU/flash/refresh tuning rather than buffer size.
    if config[CONF_VARIANT] == "cores3":
        # CoreS3 has real audio hardware (AW88298 speaker amp + ES7210 dual-mic
        # ADC on the shared duplex I2S bus). Enable the audio path in the C++
        # component (Core2's audio hardware differs, so this is cores3-only).
        cg.add_define("PATIO_AUDIO")
        # --- Acoustic echo cancellation (AEC) for wake-word barge-in: SHELVED ---
        # The idea: pull Espressif's esp-sr and enable its standalone AEC
        # (esp_aec) so the mic can cancel the AW88298's TTS playback out of its
        # own capture, letting "Okay Nabu" trigger while a reply is playing and
        # removing the ~5 s half-duplex dead window. The full implementation is
        # kept in-tree, gated on the PATIO_AEC / PATIO_DISPLAY_PSRAM defines
        # (components/patio_ui/patio_aec_ref.* + patio_microphone.cpp + the
        # bsp_display_start_with_config path in patio_ui.cpp).
        #
        # DISABLED (2026-08-23) after a hardware trial. esp_aec FD_LOW_COST needs
        # ~31 KB of INTERNAL RAM it cannot take from PSRAM, which collides with
        # LVGL's internal DMA draw buffer (boot-loop: "Not enough memory for LVGL
        # buffer (buf2) allocation!"). Moving the LVGL draw buffer to PSRAM
        # (PATIO_DISPLAY_PSRAM) freed the internal RAM and let AEC init, but the
        # PSRAM-backed flush destabilised the device: display-sleep crash + the
        # heavier flushes competing with AEC's per-frame processing starved the
        # micro_wake_word input ring (constant "Not enough free bytes... Resetting"
        # -> unreliable wake word). On this CoreS3, full-quality AEC and a stable
        # LVGL display contend for the same scarce internal RAM / core / SPI bus,
        # so we reverted to the stable half-duplex build. A future attempt could
        # try a lighter AEC (AEC_MODE_SR_LOW_COST, filter_length 2) and/or a
        # smaller draw buffer to keep the display in internal RAM.
        # cg.add_define("PATIO_AEC")
        # esp32.add_idf_component(name="espressif/esp-sr", ref="2.5.1")
        # cg.add_define("PATIO_DISPLAY_PSRAM")
        # The BSP default draw buffer is 100 rows double-buffered (~128 KB) in
        # internal DMA SRAM. With PSRAM + the IRAM/-O2 flags below there is no
        # longer a contiguous 64 KB block free, so bsp_display_start() OOMs on
        # buf1 and asserts. 40 rows double-buffered (~51 KB) fits comfortably and
        # was validated on hardware to sustain ~27 fps flings.
        esp32.add_idf_sdkconfig_option("CONFIG_BSP_LCD_DRAW_BUF_HEIGHT", 40)
        # Refresh every 10 ms instead of the 30 ms default — the single biggest
        # subjective scroll-smoothness win (LVGL only repaints once per period,
        # so 30 ms caps fling animations at ~33 fps and updates coarsely).
        esp32.add_idf_sdkconfig_option("CONFIG_LV_DEF_REFR_PERIOD", 10)
        # Put the hot LVGL draw/blend functions in IRAM so they don't run from
        # (slower) cached flash.
        esp32.add_idf_sdkconfig_option("CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM", True)
        # Build the render path with -O2 (speed) rather than the -Os default.
        # OPTIMIZATION_* is a Kconfig choice, so the default SIZE must be
        # explicitly disabled for PERF to take effect.
        esp32.add_idf_sdkconfig_option("CONFIG_COMPILER_OPTIMIZATION_SIZE", False)
        esp32.add_idf_sdkconfig_option("CONFIG_COMPILER_OPTIMIZATION_PERF", True)
