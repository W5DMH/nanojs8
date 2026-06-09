/*
 * cat.h — NanoJS8 CAT facade (Layer 6b.5)
 * =========================================
 * Profile-aware CAT controller. Inspects the active radio profile's
 * cat enum and routes operations through the right backend (CI-V for
 * Icom-family radios including the Xiegu G90; quietly does nothing
 * for CAT_NONE profiles).
 *
 * Responsibilities:
 *   - Set the serial baud to match the profile's cat_baud on start /
 *     profile change
 *   - Register a RX callback with nanojs8_usb_serial and route bytes
 *     into the CI-V parser
 *   - Hold the "last known" radio state (frequency, last response
 *     time, status) so the UI can poll it without round-tripping
 *   - Issue read-freq / set-freq commands on demand
 *   - Track timeouts (no response within N ms ⇒ status=NO_REPLY)
 *
 * What it does NOT do:
 *   - Spawn threads (operations are fire-and-forget; responses arrive
 *     on the serial RX task and update cached state)
 *   - Implement PTT (still owned by nanojs8_ptt; on G90 we use RTS
 *     not CAT for PTT, matching MicroJS8's empirical preference)
 *   - Drive mode changes or transceive monitoring (deferred to a later
 *     layer once basic freq read/set is verified on hardware)
 *
 * Status discipline:
 *   - OFF       — profile says no CAT, nothing to do
 *   - WAITING   — a request was sent; we haven't heard back yet
 *   - OK        — last response arrived within the timeout
 *   - NO_REPLY  — timeout elapsed since the last successful response
 *                 AND there's no in-flight request older than the
 *                 timeout. We don't permanently stick in NO_REPLY;
 *                 the next successful response promotes back to OK.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"   // for esp_err_t — return type of _start()
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NANOJS8_CAT_STATUS_OFF      = 0,  // profile has no CAT
    NANOJS8_CAT_STATUS_WAITING,       // request in flight
    NANOJS8_CAT_STATUS_OK,            // recent response received
    NANOJS8_CAT_STATUS_NO_REPLY,      // request timed out
} nanojs8_cat_status_t;

// Initialize the CAT subsystem. Reads the active radio profile from
// nanojs8_radio_get_active() and configures the backend. Subsequent
// profile changes flow through nanojs8_cat_apply_profile().
//
// Returns ESP_OK on success. Failures here are non-fatal — CAT can
// always come up later when a profile is selected, and CAT_NONE
// profiles are valid steady-state.
//
// Must be called AFTER nanojs8_usb_serial is started so we can
// register our RX callback. main.c sequences this correctly.
esp_err_t nanojs8_cat_start(void);

// Apply (or re-apply) a profile. Called from SETUP on commit, and
// from start() with the current active profile. Sets baud, resets
// parser state, clears cached freq, idempotent if the profile hasn't
// actually changed.
void nanojs8_cat_apply_profile(const nanojs8_radio_profile_t *profile);

// Current status. UI may call this every render — cheap.
nanojs8_cat_status_t nanojs8_cat_status(void);

// Last known radio frequency. Returns 0 if we've never received a
// valid response (e.g. CAT just initialized, profile is CAT_NONE).
// Callers should check status() before trusting this.
uint64_t nanojs8_cat_last_freq_hz(void);

// Last response time in ms since boot (esp_log_timestamp). 0 if no
// response has ever arrived. Used by status() to decide WAITING vs
// NO_REPLY.
uint32_t nanojs8_cat_last_reply_ms(void);

// Issue a read-frequency request. Non-blocking — the response (if
// any) arrives asynchronously on the serial RX path and updates the
// cached freq. The HOME 'F' hotkey calls this.
// No-op if the active profile is CAT_NONE; returns false in that case.
bool nanojs8_cat_request_freq(void);

// Issue a set-frequency command. Non-blocking. CAT_NONE profiles
// return false without sending. Hz beyond the BCD range (10 digits)
// is clamped silently — see civ_freq_to_bcd().
bool nanojs8_cat_set_freq(uint64_t freq_hz);

// L6b.6: per-tick maintenance. Called from the main loop on every
// iteration. Cheap (one atomic load most of the time). Currently
// responsible for: deferring the initial freq probe until the serial
// layer is actually ready (apply_profile can be called before USB
// enumeration completes, in which case we queue the probe instead of
// dropping it). Idempotent — safe to call as often as the caller likes.
void nanojs8_cat_tick(void);

// Diagnostic counters from the CI-V parser. Useful for the heartbeat
// log line. Counters are monotonic; reset on profile change.
void nanojs8_cat_get_counters(uint32_t *frames_ok,
                              uint32_t *frames_echoed,
                              uint32_t *frames_dropped,
                              uint32_t *tx_count);

#ifdef __cplusplus
}
#endif
