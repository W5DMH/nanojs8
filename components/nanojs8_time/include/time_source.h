/*
 * time_source.h — Manual UTC clock (Layer 7.0)
 * =====================================================
 * Operator enters UTC time-of-day via SETUP; the device computes
 * elapsed UTC by anchoring the operator's entry to esp_timer_get_time()
 * and accumulating monotonic elapsed time.
 *
 * No date tracking — JS8 slot alignment only requires (unix_seconds %
 * slot_duration), which is independent of the calendar date. The
 * operator re-enters UTC each session.
 *
 * Drift expectations
 * ==================
 * ESP32-S3 internal crystal: 40 MHz ±20 ppm typical. After one set:
 *   1 hour    → ±72 ms        well under JS8's ±100 ms tolerance
 *   2 hours   → ±144 ms       borderline; re-enter UTC before long sessions
 *   8 hours   → ±576 ms       definitely re-enter
 * Operators on extended on-air sessions should re-set UTC every hour
 * or so. L7.8 swaps this for GPS — sub-millisecond accuracy with no
 * operator action.
 *
 * Thread-safety
 * =============
 * All accessors are lock-free (atomics on anchor_us and base_seconds).
 * Set is a swap of those two atomics; reads are consistent because the
 * derivation only uses delta-from-anchor, which is monotonic.
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

// Start the time subsystem. Idempotent. Resets state to "not set"
// every boot — operator must enter UTC after every power-on.
esp_err_t nanojs8_time_start(void);

// Set the current UTC time-of-day. The device anchors this against
// esp_timer_get_time() at the moment of the call, so subsequent reads
// return the entered time plus elapsed monotonic time.
//
// Arguments are bounds-checked:
//   hour   ∈ [0, 23]
//   minute ∈ [0, 59]
//   second ∈ [0, 59]
//
// Returns ESP_ERR_INVALID_ARG if any field is out of range.
// Returns ESP_OK on success.
//
// Idempotent — calling twice just re-anchors the clock to the new
// value (operator correcting a typo, for example).
esp_err_t nanojs8_time_set_utc(uint8_t hour, uint8_t minute, uint8_t second);

// Has the operator entered UTC since boot? Used by HOME to decide
// whether to render the clock and by JS8 layers to gate slot-aligned
// TX (you cannot transmit JS8 without time sync — would miss every
// slot).
bool nanojs8_time_is_set(void);

// L7.14: which subsystem last set the clock?
//
// Used by the HOME screen to show "GPS" vs "MAN" next to the time,
// and by SETUP row 6 to optionally annotate that GPS will overwrite
// any manual entry.
typedef enum {
    NANOJS8_TIME_SOURCE_NONE   = 0,  // never set since boot
    NANOJS8_TIME_SOURCE_MANUAL = 1,  // last set via SETUP row 6
    NANOJS8_TIME_SOURCE_GPS    = 2,  // last set from GPS RMC
} nanojs8_time_source_t;

nanojs8_time_source_t nanojs8_time_get_source(void);

// L7.14: set UTC from a GPS NMEA RMC sentence. Mirrors set_utc()
// (same anchor mechanism) but always wins — GPS time is authoritative.
// Bypasses the per-call source-precedence checks because the GPS
// reader task is the only caller and the user's policy is "GPS time
// is always preferred over manual time regardless of offset".
//
// Bounds-checked identically to set_utc(). Returns ESP_ERR_INVALID_ARG
// on out-of-range fields; ESP_OK on success.
//
// Subsequent calls re-anchor the clock — used by the GPS reader's
// 60-second re-sync to keep the drift bounded over long sessions.
esp_err_t nanojs8_time_set_utc_from_gps(uint8_t hour,
                                        uint8_t minute,
                                        uint8_t second);

// Get the current UTC time-of-day. Returns false if not set yet
// (output values left unchanged in that case). Lock-free.
//
// out_hour / out_minute / out_second may be NULL if the caller only
// wants to know whether the clock is set.
bool nanojs8_time_get_utc(uint8_t *out_hour,
                          uint8_t *out_minute,
                          uint8_t *out_second);

// Seconds-since-UTC-midnight (0 .. 86399), or 0 if not set. Useful
// for slot-alignment math: (seconds_today() % 15 == 0) at JS8 Normal
// slot boundaries (00, 15, 30, 45 of each minute).
//
// NOTE: this rolls over to 0 at midnight UTC; callers performing
// long-running computations across midnight should observe the
// rollover or use a 32-bit "seconds since first set" if they need
// monotonicity.
uint32_t nanojs8_time_seconds_today(void);

// L7.1: Milliseconds-since-UTC-midnight (0 .. 86_399_999), or 0 if
// not set. Useful for slot-boundary timing where seconds-only would
// leave a sleep up to a full second imprecise. JS8 Normal slots are
// 15000 ms — millis_today() % 15000 == 0 at each boundary.
//
// NOTE: rolls over to 0 at midnight UTC, same as seconds_today().
uint32_t nanojs8_time_millis_today(void);

// Milliseconds since the operator last set UTC. Useful as a freshness
// indicator (operator should re-enter periodically) and to show
// "set 12 min ago" in the SETUP screen. Returns 0 if never set.
uint32_t nanojs8_time_age_ms(void);

#ifdef __cplusplus
}
#endif
