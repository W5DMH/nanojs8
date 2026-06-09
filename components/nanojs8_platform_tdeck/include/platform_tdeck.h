/*
 * platform_tdeck.h — LilyGO T-Deck hardware platform abstraction
 * ===============================================================
 * NanoJS8 v0.7+ platform layer.
 *
 * Provides hardware initialization for the T-Deck (regular) and T-Deck Plus.
 * Both boards use the ESP32-S3FN16R8 chip and share most peripherals; the
 * only difference is the T-Deck Plus dedicates GPIO 43/44 to its GPS module
 * while regular T-Deck exposes them on the Grove connector for general use
 * (development UART).
 *
 * This header is the public face of the nanojs8_platform_tdeck component.
 * Higher layers (UI, radio, audio) call tdeck::init() once at boot, then
 * use the bus handles via tdeck::i2c_bus() etc.
 *
 * License: GPL-3.0
 * Author: W5DMH (dan@hurdfarms.com)
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
namespace nanojs8 {
namespace platform {
namespace tdeck {

// ---------------------------------------------------------------------------
// Pin map — verified against Xinyuan-LilyGO/T-Deck examples/UnitTest/utilities.h
// Do not edit these without re-verifying against LilyGO's reference. The
// pin assignments are baked into the T-Deck PCB and are NOT configurable.
// ---------------------------------------------------------------------------

constexpr int PIN_POWERON          = 10;   // peripheral power-enable rail
constexpr int PIN_BACKLIGHT        = 42;   // TFT backlight (HIGH = on)
constexpr int PIN_I2C_SDA          = 18;   // shared bus: keyboard + touch
constexpr int PIN_I2C_SCL          = 8;
constexpr int PIN_KEYBOARD_INT     = 46;   // ESP32-C3 data-ready (falling)
constexpr int PIN_TOUCH_INT        = 16;   // GT911 interrupt
constexpr int PIN_RADIO_CS         = 9;    // SX1262 LoRa CS (HIGH = inert)
constexpr int PIN_RADIO_RST        = 17;   // SX1262 reset (LOW = held in reset)
constexpr int PIN_BAT_ADC          = 4;    // battery voltage divider, ADC1_CH3

// Display SPI pins (shared bus with SD card and LoRa); used by Layer 3
constexpr int PIN_TFT_CS           = 12;
constexpr int PIN_TFT_DC           = 11;
constexpr int PIN_SPI_MOSI         = 41;
constexpr int PIN_SPI_MISO         = 38;
constexpr int PIN_SPI_SCK          = 40;
constexpr int PIN_SDCARD_CS        = 39;

// Trackball (T-Box). Four separate direction pins plus a center-click
// button — NOT quadrature pairs. Each direction GPIO toggles state on
// every physical "tick" of the ball in that axis; we detect transitions
// (any-edge ISR) and post a virtual-key event per tick.
//
// Verified against LilyGO examples/UnitTest/utilities.h (BOARD_TBOX_G0x),
// LilyGO GitHub issue #71 (MicroPython port; mapping confirmed in user
// code), and the third-party Rust port at joshondesign.com (matches).
//
// IMPORTANT: GPIO 0 is also the chip's BOOT strapping pin. At reset
// time, LOW = download mode, HIGH = normal boot. Pulled-up internally
// after reset, so the click button works as a normal input once the
// firmware is running. Re-flash still works because the operator holds
// the trackball center BEFORE powering / pressing RST, which is the
// documented LilyGO sequence.
constexpr int PIN_TRACKBALL_LEFT   = 1;    // BOARD_TBOX_G04
constexpr int PIN_TRACKBALL_RIGHT  = 2;    // BOARD_TBOX_G01
constexpr int PIN_TRACKBALL_UP     = 3;    // BOARD_TBOX_G02
constexpr int PIN_TRACKBALL_DOWN   = 15;   // BOARD_TBOX_G03
constexpr int PIN_TRACKBALL_CLICK  = 0;    // BOARD_BOOT (shared)

// I²C device addresses (verified from LilyGO repo and Meshtastic firmware)
constexpr uint8_t I2C_ADDR_KEYBOARD = 0x55;  // ESP32-C3 keyboard coprocessor
constexpr uint8_t I2C_ADDR_TOUCH_A  = 0x5D;  // GT911 — usual address
constexpr uint8_t I2C_ADDR_TOUCH_B  = 0x14;  // GT911 — alternate address

// I²C bus parameters
constexpr int I2C_PORT             = 0;
constexpr int I2C_FREQ_HZ          = 100000;  // 100 kHz, standard mode

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialize all T-Deck platform peripherals. Idempotent — safe to call
// once at boot, no need to repeat. Logs a summary including which I²C
// devices were detected (keyboard and touch should both ACK).
//
// Sequence:
//   1. POWERON HIGH (enables everything else downstream)
//   2. Settle delay (rails come up)
//   3. Backlight ON (visual proof platform init reached this point)
//   4. LoRa SX1262 held in reset + CS HIGH (keep off shared SPI bus)
//   5. I²C master bus created on GPIO 18/8 at 100 kHz
//   6. I²C bus scan 0x08..0x77, log every address that ACKs
//
// Returns:
//   ESP_OK          on success
//   ESP_FAIL        on any unrecoverable error (bus init, GPIO config)
esp_err_t init();

// Returns the I²C master bus handle for keyboard/touch use.
// Returns nullptr if init() has not been called or failed.
i2c_master_bus_handle_t i2c_bus();

// Turn the backlight on/off. PWM control comes in Layer 3 (LovyanGFX).
// Layer 2 only does hard on/off.
void backlight_set(bool on);

} // namespace tdeck
} // namespace platform
} // namespace nanojs8
#endif // __cplusplus

// ---------------------------------------------------------------------------
// C interface for non-C++ consumers (e.g. main.c)
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tdeck_platform_init(void);
void tdeck_platform_backlight_set(bool on);

#ifdef __cplusplus
}
#endif
