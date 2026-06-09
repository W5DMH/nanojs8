/*
 * keyboard.h — NanoJS8 v0.7 keyboard input (Layer 4)
 * =====================================================
 * Reads ASCII keypresses from the LilyGO T-Deck's ESP32-C3 keyboard
 * coprocessor over I²C and makes them available to the rest of the
 * application via a FreeRTOS queue.
 *
 * Hardware facts (verified in Layer 2.5 diagnostic):
 *   - C3 keyboard at I²C address 0x55 on the shared I²C bus
 *   - Reading 1 byte returns:
 *     * The ASCII code of the most recent key, or
 *     * 0x00 if no key has been pressed since the last read
 *   - The C3 firmware handles shift internally — Shift+B sends 0x42 ('B')
 *   - Each key sends its byte ONCE per press (no auto-repeat from the C3)
 *   - 0x7F seen at other addresses (e.g. 0x40) is NOT keyboard data
 *
 * C3 boot race:
 *   The C3 firmware takes ~1 second to come up after T-Deck POWERON. Our
 *   Layer 2 I²C scan happens ~140 ms after POWERON, which means the scan
 *   often misses 0x55. By the time keyboard_start() is called from
 *   app_main (after display init, ~1.2 sec post-boot), the C3 is reliably
 *   responding — but we still build in a retry to be safe.
 *
 * Threading model:
 *   - One dedicated FreeRTOS task at priority 4, stack 4 KB
 *   - Polls 0x55 every KB_POLL_INTERVAL_MS (50 ms = 20 Hz scan rate)
 *   - Non-zero bytes go into a 16-deep queue
 *   - Consumers call nanojs8_keyboard_get_event() with a timeout
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of unread keys we'll buffer before dropping. 16 is plenty
// for a human typist; if a consumer falls behind by more than this we
// silently drop oldest events (FreeRTOS xQueueSend with 0 timeout returns
// pdFALSE and we just discard).
#define NANOJS8_KB_QUEUE_DEPTH 16

// Start the keyboard reader task. Idempotent — safe to call multiple
// times, subsequent calls are no-ops.
//
// REQUIRES platform_tdeck_init() to have succeeded (so the I²C bus
// exists). Returns ESP_OK on success, ESP_FAIL if the task or queue
// couldn't be created, ESP_ERR_INVALID_STATE if the I²C bus isn't ready.
esp_err_t nanojs8_keyboard_start(void);

// Block up to timeout_ms waiting for the next keypress. Returns the
// ASCII code of the key, or 0 if no key arrived within the timeout.
// Pass 0 for non-blocking poll, portMAX_DELAY for "wait forever".
//
// Safe to call from any task. Multiple consumers are NOT safe — only
// one task should drain the queue (otherwise events race between them).
uint8_t nanojs8_keyboard_get_key(uint32_t timeout_ms);

// Total number of keypresses received since boot. Useful for the
// heartbeat log and as a liveness signal.
uint32_t nanojs8_keyboard_total_keys(void);

// The most recent keypress, or 0 if none yet. This is a quick way to
// show "last key" on screen without consuming the queue. Updates
// whenever a key is received, regardless of whether anyone reads the
// queue.
uint8_t nanojs8_keyboard_last_key(void);

// L6b.6: True once any I²C transaction with the C3 has succeeded since
// boot. The C3 keyboard chip is intermittent at cold boot (may take
// several power-cycles to come up); operators need a clear signal for
// when it's safe to type. This flips true on the FIRST successful read,
// regardless of whether a key was pressed — i.e. it answers "is the
// chip electrically alive" not "has anyone typed yet".
//
// The check is cheap (single atomic load) — safe to call every render.
bool nanojs8_keyboard_is_alive(void);

// L6b.6: Timestamp (milliseconds since boot per esp_timer) when the
// keyboard first became alive. 0 if never. Use to show e.g.
// "Keyboard ready since +5.2s" or similar diagnostics. Most callers
// only need is_alive(); this is for the boot-log/diagnostic story.
uint32_t nanojs8_keyboard_alive_since_ms(void);

#ifdef __cplusplus
}
#endif
