/*
 * trackball.h — NanoJS8 v0.7 trackball input (Layer 6b.1)
 * ========================================================
 * Drives the T-Deck's 5-pin trackball (T-Box) as a directional input
 * source. Each physical "tick" of the ball in a cardinal direction
 * produces one virtual-key event; the trackball center click produces
 * a separate event. Events are pushed into an internal FreeRTOS queue
 * for the UI to drain.
 *
 * Hardware (verified against LilyGO examples/UnitTest/utilities.h and
 * confirmed by GitHub issue #71 / Rust port at joshondesign.com):
 *
 *   Direction  GPIO   Internal pull-up  Detection
 *   ────────   ────   ────────────────  ─────────
 *   Left       1      yes               any-edge ISR
 *   Right      2      yes               any-edge ISR
 *   Up         3      yes               any-edge ISR
 *   Down       15     yes               any-edge ISR
 *   Click      0      yes               negedge ISR (LOW = pressed)
 *
 * Why any-edge for the direction pins:
 *   The trackball is an optical encoder. Each ball-tick toggles the
 *   corresponding direction pin's state. A "rising" tick and a "falling"
 *   tick are both real ticks — we count both as movement in that
 *   direction. (Same approach as the LilyGO Rust port and Meshtastic
 *   firmware: track last_state, increment counter on any change.)
 *
 * Why an ISR per pin (not a poll loop):
 *   Polling at 20 Hz misses ticks faster than ~50 ms apart. Real
 *   users can roll the ball faster than that — the click sound from
 *   the mechanism is around 10-15 ms per tick on fast spins. An ISR
 *   captures every edge without busy-waiting and lets us debounce in
 *   software with a 3 ms minimum-interval filter to ignore mechanical
 *   contact bounce.
 *
 * Thread safety:
 *   * The ISR posts to the queue without locking; queue handles are
 *     ISR-safe per FreeRTOS guarantees.
 *   * Counters are std::atomic and ISR-safe on ESP32-S3 (32-bit aligned
 *     loads/stores are atomic on Xtensa).
 *   * nanojs8_trackball_get_event() is safe to call from any task; the
 *     queue serialises consumers, but only one task should drain.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Virtual-key codes for trackball events. Chosen above the printable
// ASCII range (0x20..0x7E) and outside the C0 control-char range
// (0x00..0x1F) the keyboard component already uses for Enter, BS, etc.
//
// 0x80-0x9F is the C1 control range — unused by the I²C keyboard, so
// these codes don't collide with anything the keyboard produces.
#define NANOJS8_TRACKBALL_UP     0x82
#define NANOJS8_TRACKBALL_DOWN   0x83
#define NANOJS8_TRACKBALL_LEFT   0x84
#define NANOJS8_TRACKBALL_RIGHT  0x85
#define NANOJS8_TRACKBALL_CLICK  0x86

// Maximum number of unread events the queue can hold before dropping.
// 16 is plenty — a human can't roll the ball faster than the UI can
// render, and dropped events on overflow just mean missed ticks (the
// UI's focus state is recoverable on the next tick).
#define NANOJS8_TRACKBALL_QUEUE_DEPTH 16

// Minimum interval (ms) between accepted events on the SAME direction
// pin. Software debounce — the mechanical contacts can bounce for a
// few microseconds on each transition, but at the ESP32-S3's 240 MHz
// clock the ISR can re-fire 100+ times within one bounce.
//
// 3 ms is comfortable margin over typical contact-bounce intervals
// (~1 ms worst case for the type of switch the trackball uses) while
// still letting real fast rolls through.
#define NANOJS8_TRACKBALL_DEBOUNCE_MS 3

// Start the trackball subsystem. Configures the 5 GPIO pins as inputs
// with internal pull-ups, installs the per-pin ISR, and creates the
// event queue.
//
// Idempotent — safe to call multiple times.
//
// REQUIRES platform_tdeck_init() to have succeeded (so POWERON has
// been driven HIGH; otherwise the trackball is dead because it's on
// the peripheral rail). Returns ESP_OK on success, ESP_FAIL on any
// GPIO config or ISR install failure.
esp_err_t nanojs8_trackball_start(void);

// Block up to timeout_ms waiting for the next trackball event. Returns
// one of the NANOJS8_TRACKBALL_* codes, or 0 if no event arrived within
// the timeout. Pass 0 for non-blocking poll, portMAX_DELAY for forever.
//
// Safe to call from any task. Multiple consumers are NOT safe — only
// one task should drain the queue.
uint8_t nanojs8_trackball_get_event(uint32_t timeout_ms);

// Total counts of each event type since boot. Useful for the heartbeat
// log and as a liveness signal. Counter is monotonic; never decreases.
// Pass any of the NANOJS8_TRACKBALL_* codes.
uint32_t nanojs8_trackball_count(uint8_t event);

#ifdef __cplusplus
}
#endif
