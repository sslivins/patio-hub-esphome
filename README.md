# patio-hub-esphome

A Sun Peaks **patio control hub** running on an **M5Stack Core2 v1.1** (ESP32,
AXP2101 PMIC, 2.0" capacitive touch). It presents a swipeable, native-feeling
touchscreen UI to control patio **heaters**, **lights**, and **screens** through
Home Assistant.

## Why "hybrid"?

ESPHome's built-in `mipi_spi` + `lvgl` stack was unacceptably laggy on this
hardware, while a native ESP-IDF + LVGL app (using `esp_lvgl_port`'s DMA flush)
is buttery smooth. This project keeps the best of both:

- A custom external component (`components/patio_ui`) **owns the LVGL layer
  directly** via the M5Stack Core2 BSP (`esp_lcd` panel + FT6336 touch +
  `esp_lvgl_port` DMA flush) — so the UI is smooth.
- **ESPHome** still provides WiFi, the native API, and OTA, and the component
  drives Home Assistant through `CustomAPIDevice`
  (`subscribe_homeassistant_state` / `call_homeassistant_service`).

## Hardware

- M5Stack Core2 **v1.1** (AXP2101 PMU — *not* the v1.0 AXP192). Primary board.
- M5Stack **CoreS3** — supported as a build target (`patio-hub-cores3.yaml`),
  bring-up in progress. See [CoreS3 migration](#cores3-migration).
- Flashed the first time over USB-C (`COM*`); subsequent updates are OTA.

## Boards / entrypoints

The whole UI + Home Assistant bridge only talks to the **generic esp-bsp API**,
so a board is chosen purely by config — no source changes:

| Entrypoint | Board | PSRAM | `patio_ui.variant` |
|---|---|---|---|
| `patio-hub.yaml` | `m5stack-core2` | `quad` | `core2` (default) |
| `patio-hub-cores3.yaml` | `m5stack-cores3` | `octal` | `cores3` |

All **board-independent** config (WiFi, API, OTA, and the `patio_ui` block with
its screens/lights/media entities) lives in `common/patio-hub-common.yaml` and
is shared by both entrypoints via `packages:`. Each entrypoint adds only the
`esp32:` board, the `psram:` mode, and (CoreS3 only) the `variant` override.
`variant` selects which esp-bsp board component is pulled in
`components/patio_ui/__init__.py` (`BSP_VARIANTS`).

## Layout

```
components/patio_ui/
  __init__.py    # ESPHome codegen: config schema, per-board BSP pull, sdkconfig/LVGL options
  patio_ui.h     # PatioUI component (Component + api::CustomAPIDevice)
  patio_ui.cpp   # LVGL UI + HA bridge (tiles, threading, service calls)
common/
  patio-hub-common.yaml   # shared, board-independent config (WiFi/API/OTA/patio_ui)
patio-hub.yaml        # Core2 entrypoint (board + PSRAM + include common)
patio-hub-cores3.yaml # CoreS3 entrypoint (board + PSRAM + variant + include common)
secrets.yaml     # (gitignored) WiFi + API key + OTA password
secrets.yaml.example
```

## The UI

A `lv_tileview` with five horizontally-swipeable tiles (Clock / Heater / Lights
/ Screens / Music):

- **Clock** — the resting/lead tile: 12-hour time with AM/PM, the weekday+date,
  and the outside temperature in °C (converted on-device from °F when the sensor
  reports Fahrenheit). After ~60 s of no touch the UI auto-reverts here — or to
  the Heater tile if a timer is running, so the countdown stays visible.
- **Heater** — live. `-5 / +5` set the run length; `Start` calls a HA script
  that starts `timer.patio_heaters`; the tile counts down locally at 1 Hz,
  synced from the HA timer's `remaining` attribute; `Stop` stops it.
- **Screens** — live. An *egocentric* perimeter map (as seen standing in front
  of the Core2): `Left` / `Right` single side screens, plus the two side-by-side
  `Rear Left` / `Rear Right` screens on the wall behind you, with `ALL` in the
  middle. Tap tiles (or `ALL`) to build a selection, then `Open` / `Close` /
  `Stop` acts on it. Each tile shows live cover state (`Open`/`Closed`/`NN%`/
  `Opening`/`Closing`). Configure the 4 slots (`left`, `right`, `rear_left`,
  `rear_right`) under `patio_ui.screens` in the YAML; each takes an `entity_id`
  (a `cover.*`) and an optional `label`.
- **Lights** — live. Two vertical dim faders (`Main` / `BBQ`); tap a group name
  to toggle on/off, drag its fader to set brightness (0 = off). Bidirectional:
  external HA changes are reflected back onto the faders. Configure the slots
  under `patio_ui.lights` (each `entity_id` a `light.*` + optional `label`).
- **Music** — live. Controls a single deck Sonos amp (`media_player.*`): a
  now-playing line, a horizontal volume fader, and prev / play-pause / next.
  Bidirectional (state/volume/title push back); the play/pause icon flips
  optimistically on tap. Configure under `patio_ui.media` (`entity_id` +
  optional `label`).

### Threading model (lock-free)

- All LVGL objects are touched **only** from the `esp_lvgl_port` task: button
  event callbacks and a 1 Hz `lv_timer`.
- Home Assistant state callbacks and `Component::loop()` run on the ESPHome
  main/API task and **only** touch `std::atomic` members.
- UI → HA intents are queued in atomics and drained in `loop()`; HA → UI updates
  set atomics that the `lv_timer` reads and renders. No cross-task LVGL access.

## Display BSP

The display is brought up by the M5Stack Core2 BSP, pulled at build time from a
fork that (a) uses the **new i2c-ng** master driver so FT6336 touch init works
under ESP-IDF ≥ 5.5, and (b) includes a **Core2 v1.1 AXP2101 ALDO2 LCD/touch
reset** fix so the panel isn't blank. See the `ESP_BSP_*` constants in
`components/patio_ui/__init__.py`.

## Build & flash

Requires [ESPHome](https://esphome.io/) (tested with 2026.7.4, ESP-IDF 5.5.5).

```bash
cp secrets.yaml.example secrets.yaml   # then fill in real values

# Core2 (primary):
esphome compile patio-hub.yaml
esphome upload  patio-hub.yaml --device COM15   # first flash over USB
esphome logs    patio-hub.yaml --device COM15

# CoreS3 (bring-up): same commands against the sibling entrypoint
esphome run patio-hub-cores3.yaml --device COM<N>
```

After the device is on WiFi, add it to Home Assistant's **ESPHome** integration
(auto-discovered as `patio-hub`, noise key = `core2_patio_api_key`). Enable
**"Allow the device to perform Home Assistant actions"** on the device so the
outbound `Open`/`Close`/`Stop`/`Start`/`Stop` service calls work.

## Live screen capture

The component runs a tiny HTTP server on **port 8080** that renders the live
LVGL framebuffer to an uncompressed PNG (via `lv_snapshot`), handy for UX
iteration and CI without a physical photo:

```bash
curl http://<device-ip>:8080/screenshot -o shot.png
```

It captures whatever tile is currently on screen. Encoding is uncompressed
(~230 KB for 320×240, <1 s) to keep CPU cost trivial. The PNG encoder
(`png_uncompressed.c/.h`) is borrowed from the arctic-controller project.

## Home Assistant contract

- `timer.patio_heaters` + `script.patio_heater_run` (`{minutes}`) +
  `script.patio_heater_stop` for the heater tile.
- Any four `cover.*` entities for the screen tile. The screens are driven as
  Somfy RTS (command-only): the tiles are selectors and the control bar sends
  momentary up / stop / down commands — no state is read back.

## CoreS3 migration

The CoreS3 shares the Core2's 320×240 panel and AXP2101 PMU, so the entire UI
layer compiles for it unchanged — only the underlying BSP differs. Both targets
already build clean under our IDF, so the remaining work is purely **runtime
bring-up on the physical board**.

### Bring-up checklist

1. **First flash over wired USB-C** (a blank board can't OTA):
   ```bash
   esphome run patio-hub-cores3.yaml --device COM<N>
   ```
   The CoreS3's USB-C goes straight to the ESP32-S3. If the port isn't detected,
   hold **RST** briefly / re-plug to enter download mode.
2. **Panel lights up** (no blank screen). If blank, it's BSP power sequencing —
   on the CoreS3 the LCD reset/backlight are behind an **AW9523 GPIO expander**
   (+ AXP2101), unlike the Core2's ALDO2 rail. Check the log around
   `bsp_display_start`; if broken, point the `cores3` entry's `repo`/`ref` in
   `components/patio_ui/__init__.py` (`BSP_VARIANTS`) at a dedicated fork branch
   with a board fix — same pattern used for the Core2 v1.1 reset fix.
3. **Touch works** — swipe between the five tiles (Clock / Heater / Lights /
   Screens / Music). CoreS3 touch is FT6336-family (same driver family as Core2).
4. **Status LED off** — verify the `AXP2101 CHGLED (reg 0x69)` silence in
   `setup()` still kills the side LED. The CoreS3 power/charge indicator may be
   wired via the **AW9523** instead of the AXP CHGLED; if the LED still blinks,
   the off-write needs to target the AW9523 output register instead.
5. **WiFi + API + OTA** — the device joins WiFi and is auto-discovered in HA's
   ESPHome integration as **`patio-hub-s3`** (distinct name so it doesn't clash
   with the live Core2). Enable **"Allow the device to perform Home Assistant
   actions"** so outbound service calls work.
6. **Exercise every tile** — clock/outside-temp, heater timer (start/stop/+15),
   screens up/stop/down, light faders, and the Sonos volume + transport.
7. **OTA works** — reflash over the network:
   `esphome run patio-hub-cores3.yaml` (no `--device`).
8. **Screenshot endpoint** — `curl http://<device-ip>:8080/screenshot -o shot.png`.
9. **Cut over** (optional) — once validated, rename the entrypoint's `name`
   back to `patio-hub` to have the CoreS3 permanently replace the Core2.

### Planned CoreS3-only feature: wake the screen on approach

Goal: dim/sleep the backlight when idle and wake it when someone approaches
(target ≈ a couple of feet).

**Sensor reality check — the onboard proximity won't reach a couple feet.** The
CoreS3's built-in **LTR-553ALS** is an ambient-light + *reflective IR* proximity
sensor whose proximity mode only detects objects within **a few centimetres**
(it's the "phone-to-ear" class of sensor). It's fine for a deliberate hand-wave
right in front of the glass, but not for detecting an approach across the deck.

For couple-feet wake-on-approach, add an **external presence sensor** on a Grove
port (this also sidesteps the internal-I²C ownership issue below):

| Option | Range | Notes |
|---|---|---|
| **mmWave LD2410** (UART/Grove) — *recommended* | ~0.75–6 m, gated per-gate | Detects **presence** (incl. stationary) with tunable distance zones; ideal for "someone is on the deck". ESPHome has an `ld2410` component. |
| **PIR** (AS312 / HC-SR501, GPIO/Grove) | several m | Cheap, detects **motion** only (not a stationary person); a person standing still will time out. |

**Firmware work when the sensor is in hand:**
- Add a **backlight-sleep** step: after the existing idle window, drop the
  backlight (`bsp_display_brightness_set(0)` / `bsp_display_backlight_off()`)
  instead of only auto-reverting the tile.
- Add a **wake** path: an ESPHome `binary_sensor` (LD2410 presence or PIR) whose
  `on_press` calls into `patio_ui` to restore the backlight and reset the idle
  timer. The inactivity plumbing already exists (`maybe_auto_revert_` /
  `lv_display_get_inactive_time`), so this is an extension, not a rewrite.
- **I²C-ownership caveat for the *onboard* LTR-553:** it sits on the internal
  I²C bus that the `patio_ui` component owns exclusively (we deliberately declare
  no top-level `i2c:`). Reading it via a normal ESPHome `i2c` sensor would fight
  that ownership — it would have to be read *inside* the component via
  `bsp_i2c_get_handle()`. An external Grove sensor on its own GPIO/UART avoids
  the conflict entirely, which is a second reason to prefer mmWave/PIR.

## Status / TODO

- [x] Smooth native LVGL UI inside ESPHome
- [x] Clock + outside-temperature lead tile (12h, °C, idle auto-revert)
- [x] Heater tile wired to a HA timer (end-to-end verified)
- [x] Lights tile: dual dim faders, bidirectional with HA
- [x] Screens tile: perimeter map + iconic selectors + up/stop/down (HA→device verified)
- [x] Music tile: deck Sonos volume + play/pause/prev/next (bidirectional)
- [x] Live screen-capture PNG endpoint (`:8080/screenshot`)
- [x] CoreS3 build target (variant switch, shared package) — compiles clean
- [ ] CoreS3 runtime bring-up (panel/touch/LED) — needs hardware
- [ ] Wake-on-approach via external presence sensor (mmWave/PIR) + backlight sleep
- [ ] Point screen slots at the real Sun Peaks Somfy screen covers
