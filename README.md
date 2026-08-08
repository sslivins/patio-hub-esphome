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

- M5Stack Core2 **v1.1** (AXP2101 PMU — *not* the v1.0 AXP192).
- Flashed the first time over USB-C (`COM*`); subsequent updates are OTA.

## Layout

```
components/patio_ui/
  __init__.py    # ESPHome codegen: config schema, BSP pull, sdkconfig/LVGL options
  patio_ui.h     # PatioUI component (Component + api::CustomAPIDevice)
  patio_ui.cpp   # LVGL UI + HA bridge (tiles, threading, service calls)
patio-hub.yaml   # top-level ESPHome config
secrets.yaml     # (gitignored) WiFi + API key + OTA password
secrets.yaml.example
```

## The UI

A `lv_tileview` with three horizontally-swipeable tiles:

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
- **Lights** — static placeholder (to be wired next).

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
esphome compile patio-hub.yaml
esphome upload  patio-hub.yaml --device COM15   # first flash over USB
esphome logs    patio-hub.yaml --device COM15
```

After the device is on WiFi, add it to Home Assistant's **ESPHome** integration
(auto-discovered as `patio-hub`, noise key = `core2_patio_api_key`). Enable
**"Allow the device to perform Home Assistant actions"** on the device so the
outbound `Open`/`Close`/`Stop`/`Start`/`Stop` service calls work.

## Home Assistant contract

- `timer.patio_heaters` + `script.patio_heater_run` (`{minutes}`) +
  `script.patio_heater_stop` for the heater tile.
- Any four `cover.*` entities for the screen tile (open/close/stop; position
  optional but used for the `NN%` readout).

## Status / TODO

- [x] Smooth native LVGL UI inside ESPHome
- [x] Heater tile wired to a HA timer (end-to-end verified)
- [x] Screens tile: perimeter map + select + Open/Close/Stop (HA→device verified)
- [ ] UX polish (some labels clip; tune sizes/fonts)
- [ ] Wire the Lights tile
- [ ] Point screen slots at the real Sun Peaks Caseta screen covers
