// ============================================================================
// axp_probe.h  —  minimal AXP2101 access for the deep-sleep battery probe
//
// Standalone (no esp-bsp / no patio_ui) helper used ONLY by patio-sleeptest.yaml.
// Talks to the Core2 v1.1 AXP2101 PMU at 0x34 on the internal I2C bus
// (SDA=GPIO21, SCL=GPIO22) with the ESP-IDF new-i2c master API — the exact
// calls already proven in patio_ui.cpp::poll_power_status_().
//
// Register cheat-sheet (AXP2101, verified against esp-bsp m5stack_core_2.c):
//   0x00        STATUS1     bit5 = VBUS present (on battery == bit clear)
//   0x30        ADC ctrl    bit0 = enable battery-voltage ADC
//   0x34/0x35   VBAT ADC    H6L8 -> millivolts directly
//   0x69        CHGLED      0x00 = manual, output off (kills the side LED)
//   0x90        LDO enable  bit4 (0x10) = BLDO1 = LCD backlight rail
//   0xA4        fuel gauge  battery percent 1..100 (0/255 = gauge disabled)
//
// We only ever CUT BLDO1 (backlight). Every other rail on 0x90 — notably
// ALDO2 (bit1, the LCD/touch reset rail) — is left exactly as the running
// firmware set it, so the FT6336 touch controller stays powered and can wake
// the ESP from deep sleep via its INT line (GPIO39), and there's no brownout.
// ============================================================================
#pragma once

#include "driver/i2c_master.h"
#include "driver/gpio.h"

namespace axp_probe {

// Lazily bring up the internal I2C bus + AXP2101 device once per boot. Returns
// the device handle (or nullptr on failure). After a deep-sleep wake the ESP
// cold-boots, so this re-runs cleanly each cycle.
inline i2c_master_dev_handle_t dev() {
  static i2c_master_dev_handle_t s_axp = nullptr;
  if (s_axp != nullptr)
    return s_axp;

  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = 0;                       // I2C_NUM_0 (unused by anything else here)
  bus_cfg.sda_io_num = GPIO_NUM_21;           // Core2 internal I2C SDA
  bus_cfg.scl_io_num = GPIO_NUM_22;           // Core2 internal I2C SCL
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK)
    return nullptr;

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = 0x34;              // AXP2101
  dev_cfg.scl_speed_hz = 100000;
  if (i2c_master_bus_add_device(bus, &dev_cfg, &s_axp) != ESP_OK) {
    s_axp = nullptr;
    return nullptr;
  }
  return s_axp;
}

// Read one 8-bit register (-1 on failure).
inline int read_reg(uint8_t reg) {
  auto d = dev();
  if (!d)
    return -1;
  uint8_t v = 0;
  if (i2c_master_transmit_receive(d, &reg, 1, &v, 1, 200) != ESP_OK)
    return -1;
  return v;
}

// Write one 8-bit register.
inline void write_reg(uint8_t reg, uint8_t val) {
  auto d = dev();
  if (!d)
    return;
  uint8_t w[2] = {reg, val};
  i2c_master_transmit(d, w, 2, 200);
}

// Battery voltage in millivolts (regs 0x34/0x35, H6L8). -1 on failure.
inline int batt_mv() {
  auto d = dev();
  if (!d)
    return -1;
  uint8_t rh = 0x34, rl = 0x35, vh = 0, vl = 0;
  if (i2c_master_transmit_receive(d, &rh, 1, &vh, 1, 200) != ESP_OK)
    return -1;
  if (i2c_master_transmit_receive(d, &rl, 1, &vl, 1, 200) != ESP_OK)
    return -1;
  return ((vh & 0x3F) << 8) | vl;
}

// Fuel-gauge percent (reg 0xA4). -1 if the gauge is disabled (0/255).
inline int batt_pct() {
  int g = read_reg(0xA4);
  return (g >= 1 && g <= 100) ? g : -1;
}

// True when running on battery (VBUS absent, STATUS1 bit5 clear).
inline bool on_battery() {
  int s = read_reg(0x00);
  return (s >= 0) && ((s & 0x20) == 0);
}

// Enable the battery-voltage ADC (reg 0x30 bit0) so batt_mv() is valid.
inline void enable_batt_adc() {
  int cur = read_reg(0x30);
  if (cur < 0)
    return;
  write_reg(0x30, (uint8_t)(cur | 0x01));
}

// Cut the LCD backlight rail (reg 0x90 bit4 / BLDO1) — the true rail-off state,
// same as esp-bsp bsp_display_brightness_set(0). Read-modify-write so no other
// rail is touched.
inline void backlight_off() {
  int cur = read_reg(0x90);
  if (cur < 0)
    return;
  uint8_t desired = (uint8_t)(cur & ~0x10);
  if (desired != (uint8_t)cur)
    write_reg(0x90, desired);
}

// DEEPEST-SLEEP variant: cut BOTH the backlight (BLDO1, bit4) AND the
// LCD/touch reset rail (ALDO2, bit1) on reg 0x90. This powers down the FT6336
// touch controller and LCD panel entirely, so there is NO touch-wake and no
// panel draw during sleep — the device only comes back via the physical RESET
// button (a full reboot). Read-modify-write so unrelated rails are untouched.
inline void panel_and_touch_off() {
  int cur = read_reg(0x90);
  if (cur < 0)
    return;
  uint8_t desired = (uint8_t)(cur & ~0x12);  // clear bit4 (BLDO1) + bit1 (ALDO2)
  if (desired != (uint8_t)cur)
    write_reg(0x90, desired);
}

// Silence the side charge-indicator LED (reg 0x69 -> manual, off).
inline void silence_led() { write_reg(0x69, 0x00); }

}  // namespace axp_probe
