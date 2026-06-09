/*
 * gps.h — u-blox MIA-M10Q GNSS receiver (L7.14)
 * ==============================================
 * Hardware-only on the LilyGO T-Deck Plus: the M10Q module is on
 * GPIO 43/44 at 38400 baud, NMEA 4.11.
 *
 * Conditional on CONFIG_NANOJS8_GPS_ENABLED (default off). When the
 * Kconfig option is OFF, this component still compiles but the
 * functions below become no-ops returning their "disabled" defaults,
 * which keeps the rest of the firmware free of #ifdef sprawl.
 *
 * What it does
 * ============
 * After app_main hands off the UART0 console pins, this component:
 *   1. Brings up UART1 with RX on GPIO 44 (and TX idle-high on 43)
 *   2. Reads NMEA bytes into a 128-byte line buffer
 *   3. Parses $G?RMC sentences (RMC has UTC time + date + fix valid)
 *   4. On first valid fix: nanojs8_time_set_utc_from_gps()
 *   5. Re-syncs every 60 s while fix held (bounds crystal drift)
 *   6. Exposes status (NO_FIX/SEARCHING/LOCKED) + fix age for HOME row
 *
 * The receive-only design is deliberate: we never send commands to
 * the M10Q (no UBX configuration, no rate change). Default 1 Hz
 * NMEA output and 38400 baud are exactly what we want. UART1 TX is
 * configured (pin 43) so that pin idles high — keeps the M10Q's RX
 * line in a clean state. We never call uart_write_bytes().
 *
 * Console handoff
 * ===============
 * Caller (main.c) is responsible for tearing down UART0 console and
 * gpio_reset_pin'ing 43/44 BEFORE calling nanojs8_gps_init(). The
 * init function assumes the pins are free.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Status the HOME screen renders.
typedef enum {
    NANOJS8_GPS_DISABLED  = 0,   // Kconfig option is OFF
    NANOJS8_GPS_NO_FIX    = 1,   // module not detected (no bytes seen)
    NANOJS8_GPS_SEARCHING = 2,   // bytes arriving but RMC status = V
    NANOJS8_GPS_LOCKED    = 3,   // last RMC was valid (status = A)
} nanojs8_gps_status_t;

// Snapshot of GPS state for the UI to render. Populated atomically by
// nanojs8_gps_get_status; consistent within a single call but may
// change between calls.
typedef struct {
    nanojs8_gps_status_t status;
    uint32_t last_rmc_age_ms;    // ms since last RMC (any status); UINT32_MAX if never
    uint32_t last_fix_age_ms;    // ms since last VALID RMC; UINT32_MAX if never
    uint8_t  hour;               // last fix UTC, only valid when status==LOCKED
    uint8_t  minute;
    uint8_t  second;
    uint32_t total_sentences;    // diagnostic: total $G?RMC seen
    uint32_t valid_fixes;        // diagnostic: total status=A seen
    uint32_t parse_errors;       // diagnostic: checksum/format errors
    // L7.14-fix6: last known position. pos_valid latches true on the
    // first valid RMC with parsable lat/lon and stays true thereafter,
    // so a brief loss of fix doesn't immediately blank MYLOC's coords.
    // Microdegrees (signed): positive lat = N, positive lon = E.
    // Range: lat ±90_000_000, lon ±180_000_000 — fits in int32_t with
    // headroom.
    bool     pos_valid;
    int32_t  lat_microdeg;
    int32_t  lon_microdeg;
} nanojs8_gps_snapshot_t;

// Compile-time check on CONFIG_NANOJS8_GPS_ENABLED. Lets callers do
// `if (nanojs8_gps_is_enabled()) { ... }` without #ifdef. The compiler
// folds the constant and dead-strips the branch when OFF.
static inline bool nanojs8_gps_is_enabled(void) {
#ifdef CONFIG_NANOJS8_GPS_ENABLED
    return true;
#else
    return false;
#endif
}

// Bring up UART1 RX on GPIO 44 + spawn the reader task. Caller must
// have already (a) drained pending log output, (b) torn down UART0
// console, (c) reset GPIO 43/44 — see main.c's console-handoff
// block. Idempotent; returns ESP_OK on first call, ESP_ERR_INVALID_STATE
// if already initialized. When CONFIG_NANOJS8_GPS_ENABLED=n, returns
// ESP_OK without doing anything.
esp_err_t nanojs8_gps_init(void);

// Populate *out with the current snapshot. Safe to call from any task.
// When CONFIG_NANOJS8_GPS_ENABLED=n, sets out->status = DISABLED and
// zeroes the rest.
void nanojs8_gps_get_snapshot(nanojs8_gps_snapshot_t *out);

// L7.14-fix6: format the last known GPS position as signed decimal
// degrees, "lat,lon" with 4 decimal places — e.g. "42.3601,-71.0589"
// (about 11 m precision, plenty for a radio "where I am" message).
// Writes a NUL-terminated string into buf.
//
// Returns true if buf was filled with a meaningful position.
// Returns false (and writes "" to buf if buf_n > 0) when no valid
// fix has been seen this session, status is DISABLED/NO_FIX, or the
// position parse failed for some reason. Callers should banner an
// error and not put garbage on the wire.
//
// Used by COMPOSE's MYLOC verb to auto-populate the TEXT field.
bool nanojs8_gps_format_position(char *buf, size_t buf_n);

// Standalone parser self-test on fixed NMEA strings (no UART
// involved). Verifies:
//   - checksum validation accepts known-good and rejects flipped bits
//   - RMC parser extracts time/date/status correctly
//   - bad inputs are rejected without overrun
//
// Returns true on PASS. Logs each subtest result. Always available
// regardless of CONFIG_NANOJS8_GPS_ENABLED, so the encoder self-test
// at boot still exercises the parser code path.
bool nanojs8_gps_self_test(void);

#ifdef __cplusplus
}
#endif
