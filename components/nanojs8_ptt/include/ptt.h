/*
 * ptt.h — NanoJS8 v0.7 PTT controller (Layer 6b.4)
 * ====================================================
 * Push-to-talk service. Sits above nanojs8_usb_serial (which exposes
 * raw RTS/DTR control) and below the future modem layer. The PTT
 * controller's job is to take a simple `set(true|false)` API and
 * translate it into the right hardware action for the currently
 * selected radio profile, with an inviolable safety watchdog.
 *
 * Responsibilities
 * ────────────────
 *   1. At start, read the active radio profile and tell the serial
 *      layer which line is PTT (RTS / DTR / none).
 *   2. Provide a single `nanojs8_ptt_set(bool)` API that drives the
 *      configured hardware line.
 *   3. Track PTT state (asserted? for how long?) so the UI can show it.
 *   4. Run a watchdog task that force-releases PTT if it stays
 *      asserted longer than NANOJS8_PTT_WATCHDOG_MS. This is a SAFETY
 *      feature: a stuck PTT can blow up a finals, exceed band-plan
 *      duty cycle, or just waste battery. 20 s is comfortable headroom
 *      above the longest legitimate JS8 Normal frame (12.64 s).
 *   5. Apply a new profile on the fly when the operator changes
 *      radios in SETUP (re-configure serial PTT line, log it).
 *
 * What this component does NOT do
 * ───────────────────────────────
 *   - No transmit gating based on band plan, duty cycle, or audio
 *     buffer state — that's the modem layer's job.
 *   - No CAT commands. TX/RX via CAT is a future profile option but
 *     L6b.4 has none of the profiles using it.
 *   - No de-bouncing or rate limiting on toggle. set(true) immediately
 *     keys the radio. The watchdog is the only automatic action.
 *
 * Thread safety
 * ─────────────
 *   nanojs8_ptt_set() is callable from any task. The watchdog task
 *   runs at low priority with a 250 ms polling cadence. State variables
 *   are std::atomic so the status row in HOME can read without locking.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Default PTT-assertion ceiling (also the ceiling at boot before any
// burst). The watchdog force-releases PTT if it stays asserted longer
// than this. 20 s gives ~7 s of headroom over JS8 Normal's 12.64 s
// single-frame audio, which is the right safety net for a single-frame
// TX. Multi-frame TX (continuous-PTT bursts spanning N slots) bumps
// the limit for the duration of the burst via
// nanojs8_ptt_set_burst_watchdog_ms() — see below.
#define NANOJS8_PTT_WATCHDOG_MS 20000

// L7.11f-fix2: bump the PTT-assertion ceiling for the NEXT release-
// gated window only. The bumped limit is CONSUMED on the next PTT
// release, automatically reverting to NANOJS8_PTT_WATCHDOG_MS. This is
// the MicroJS8 "continuous PTT across multi-frame burst" pattern: the
// caller computes (n_frames × NANOJS8_PTT_WATCHDOG_MS + 5000) ms, calls
// this once before keying PTT, runs the whole burst with PTT held, then
// releases PTT — and the limit auto-resets to default.
//
// Safety:
//   * If a caller bumps the limit and then crashes mid-burst with PTT
//     keyed, the bumped watchdog still fires (eventually) and releases
//     PTT — and that release itself clears the bump. The next legit
//     single-frame TX runs with default 20 s.
//   * Callers can EXTEND the limit but cannot SHORTEN it: requests
//     below NANOJS8_PTT_WATCHDOG_MS are rejected with a WARN log. The
//     default is a floor.
//   * Pass 0 to explicitly clear any previously-set bump without doing
//     a PTT cycle (useful in worker abort paths between bump and key).
//
// Thread-safe; can be called from any task.
void nanojs8_ptt_set_burst_watchdog_ms(uint32_t limit_ms);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialize the PTT subsystem. Reads the active radio profile via
// nanojs8_radio_get_active() and pushes its PTT mechanism down to the
// serial layer. Starts the watchdog task. Idempotent.
//
// MUST be called AFTER nanojs8_config_init(), nanojs8_serial_start(),
// because both are read at init time.
esp_err_t nanojs8_ptt_start(void);

// Re-apply a (potentially new) radio profile. Called from the SETUP
// screen's commit path when the operator changes the Radio row. If
// PTT is currently asserted, it is force-released BEFORE switching
// profiles — switching the PTT line under a live transmission would
// leave the old line stuck high.
esp_err_t nanojs8_ptt_apply_profile(const nanojs8_radio_profile_t *profile);

// ---------------------------------------------------------------------------
// PTT control
// ---------------------------------------------------------------------------

// Key (true) or release (false) PTT using the active profile's line.
// If the active profile's ptt == NONE, the call succeeds but does
// nothing (and emits a one-time WARN log). The watchdog timer is
// restarted on every assert.
//
// Returns ESP_OK on success, ESP_ERR_INVALID_STATE if not started.
esp_err_t nanojs8_ptt_set(bool transmitting);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// True if PTT is currently asserted (radio keyed).
bool nanojs8_ptt_is_keyed(void);

// If PTT is asserted, returns milliseconds since the assert. If
// released, returns 0. Useful for the HOME status row's countdown.
uint64_t nanojs8_ptt_keyed_ms(void);

// Total number of PTT cycles (release events) since boot. Used as a
// reliability counter — flaky PTT will accumulate spurious cycles.
uint32_t nanojs8_ptt_total_tx(void);

// Total number of watchdog-induced releases since boot. Should be 0
// in normal operation. Non-zero means something kept PTT asserted past
// the limit — either a bug in the modem layer or a wedged caller.
uint32_t nanojs8_ptt_watchdog_trips(void);

#ifdef __cplusplus
}
#endif
