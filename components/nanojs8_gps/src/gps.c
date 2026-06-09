/*
 * gps.c — u-blox MIA-M10Q NMEA reader (L7.14)
 * ============================================
 * See gps.h for high-level overview.
 *
 * Implementation notes
 * ====================
 * The reader task pattern:
 *   1. uart_read_bytes(UART1, buf, 64, 100ms) — short timeout so the
 *      task wakes every ~100 ms to update last_byte_us even when no
 *      data is arriving. This is what flips status from NO_FIX to
 *      SEARCHING ("bytes were arriving but no valid fix yet").
 *   2. Walk the byte buffer; on '\n', terminate the line buffer and
 *      hand to handle_line(). Skip '\r'. Discard the line if it
 *      overflows the 128-byte buffer.
 *   3. handle_line(): early-exit if not starting with '$'. Validate
 *      checksum. If RMC sentence, parse and update state. Other
 *      sentence types (GGA, VTG, GLL, ...) are silently ignored.
 *
 * Re-sync timing
 * ==============
 * First valid RMC sets the clock immediately. Subsequent valid RMCs
 * only trigger set_utc_from_gps() if 60 s have elapsed since the
 * last sync. This bounds drift contributions without thrashing the
 * time-component atomics on every 1 Hz update.
 *
 * Status state machine
 * ====================
 *   start                       → NO_FIX
 *   first byte received         → SEARCHING (if no fix yet)
 *   RMC with status='A' parsed  → LOCKED
 *   No bytes in last 5 seconds  → NO_FIX (module disconnected?)
 *
 * Thread safety
 * =============
 * The snapshot is consumed by the UI render task and produced by the
 * reader task. We use individual atomics for each field; reads may
 * see a slightly torn struct (e.g. status updated but age stale by a
 * few ms) which is harmless for display.
 *
 * License: GPL-3.0
 */

#include "gps.h"

#include "time_source.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // xTaskCreatePinnedToCoreWithCaps

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "gps";

// ── Hardware wiring (T-Deck Plus) ─────────────────────────────────────
#define GPS_UART_NUM       UART_NUM_1
#define GPS_UART_TX_PIN    43      // idle-high; we never transmit
#define GPS_UART_RX_PIN    44
#define GPS_UART_BAUD      38400
#define GPS_UART_RX_BUF    1024    // 80 B/sec NMEA, 1 KB gives ~12 s headroom

// ── Tuning ────────────────────────────────────────────────────────────
#define LINE_BUF_MAX       128     // NMEA sentence max is 82; 128 is comfortable
#define RX_READ_CHUNK      64
#define RX_READ_WAIT_MS    100     // task wake every 100 ms for status updates
#define RESYNC_INTERVAL_US (60ULL * 1000 * 1000)  // 60-second re-sync
#define STALE_BYTES_US     (5ULL  * 1000 * 1000)  // 5 s with no bytes → NO_FIX
#define STALE_FIX_US       (30ULL * 1000 * 1000)  // 30 s with no valid RMC → LOCKED → SEARCHING

// Task stack: parser logic is tight, but xTaskCreatePinnedToCoreWithCaps
// requires the stack in PSRAM (the WithCaps cap flags), and PSRAM stacks
// are byte-addressable. 4 KB is plenty for our string handling.
#define GPS_TASK_STACK     4096
#define GPS_TASK_PRIORITY  4
#define GPS_TASK_CORE      tskNO_AFFINITY
#define GPS_TASK_NAME      "nanojs8_gps"

// ── State ─────────────────────────────────────────────────────────────
//
// Individual atomics rather than a single mutex-guarded struct: each
// field is updated independently by the reader, and the UI snapshot
// tolerates being slightly torn (worst case: status says LOCKED but
// last_fix_age is from 100 ms ago — visually identical to consistent
// state).
static _Atomic int      s_status        = NANOJS8_GPS_NO_FIX;
static _Atomic uint64_t s_last_byte_us  = 0;   // last time ANY byte arrived
static _Atomic uint64_t s_last_rmc_us   = 0;   // last RMC parsed
static _Atomic uint64_t s_last_fix_us   = 0;   // last RMC with status='A'
static _Atomic uint64_t s_last_sync_us  = 0;   // last call to set_utc_from_gps
static _Atomic uint8_t  s_last_fix_h    = 0;
static _Atomic uint8_t  s_last_fix_m    = 0;
static _Atomic uint8_t  s_last_fix_s    = 0;
static _Atomic uint32_t s_total_rmc     = 0;
static _Atomic uint32_t s_valid_fixes   = 0;
static _Atomic uint32_t s_parse_errors  = 0;
static _Atomic bool     s_initialized   = false;
// L7.14-fix6: last-known position from a valid RMC. Microdegrees in
// int32 (≈11 cm precision). pos_valid latches true on first valid
// fix and stays true thereafter — a short fix outage doesn't blank
// MYLOC's coords. Individual atomics; same race tolerance as the
// time fields (UI may see a momentarily torn lat/lon, harmless for
// display and rare in practice).
static _Atomic bool    s_last_fix_pos_valid = false;
static _Atomic int32_t s_last_fix_lat_microdeg = 0;
static _Atomic int32_t s_last_fix_lon_microdeg = 0;

// ── NMEA parsing ──────────────────────────────────────────────────────

// XOR all bytes between '$' and '*'. Returns 0 if no '*' or no '$',
// or the checksum byte. Caller compares to the two hex digits after
// the '*'.
static uint8_t nmea_compute_checksum(const char *line) {
    if (!line || line[0] != '$') return 0;
    uint8_t cs = 0;
    for (const char *p = line + 1; *p && *p != '*'; ++p) {
        cs ^= (uint8_t)*p;
    }
    return cs;
}

// Returns true if the line has a '*XX' suffix where XX matches the
// computed checksum. Tolerates lowercase hex. Length-bounded — does
// not read past the '\0'.
static bool nmea_checksum_ok(const char *line) {
    if (!line || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star || star[1] == '\0' || star[2] == '\0') return false;

    // Parse two hex digits after the '*'.
    char a = star[1];
    char b = star[2];
    uint8_t hi = (a >= '0' && a <= '9') ? (a - '0') :
                 (a >= 'A' && a <= 'F') ? (a - 'A' + 10) :
                 (a >= 'a' && a <= 'f') ? (a - 'a' + 10) : 0xFF;
    uint8_t lo = (b >= '0' && b <= '9') ? (b - '0') :
                 (b >= 'A' && b <= 'F') ? (b - 'A' + 10) :
                 (b >= 'a' && b <= 'f') ? (b - 'a' + 10) : 0xFF;
    if (hi == 0xFF || lo == 0xFF) return false;
    return (uint8_t)((hi << 4) | lo) == nmea_compute_checksum(line);
}

// Convert two ASCII digits "DD" to 0..99. Returns -1 on non-digit.
static int two_digits(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

// L7.14-fix6: parse NMEA lat "DDMM.MMMM" + hemi (N/S) into signed
// microdegrees. Returns true on success. Field must start with 4
// digit chars; the minutes part is parsed with strtod, which stops
// at the first non-numeric char (the trailing comma is what stops it).
static bool parse_nmea_lat(const char *s, char hemi, int32_t *out) {
    if (!s) return false;
    if (hemi != 'N' && hemi != 'S') return false;
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;

    const int dd = two_digits(s);
    if (dd < 0 || dd > 89) return false;

    // Minutes substring starts at s[2]; strtod stops at the comma.
    char *endp = NULL;
    const double minutes = strtod(s + 2, &endp);
    // Require that strtod actually consumed something — endp moved past s+2.
    if (endp == s + 2) return false;
    if (minutes < 0.0 || minutes >= 60.0) return false;

    double deg = (double)dd + minutes / 60.0;
    if (hemi == 'S') deg = -deg;

    // Round to nearest microdegree.
    *out = (int32_t)(deg * 1000000.0 + (deg >= 0.0 ? 0.5 : -0.5));
    return true;
}

// L7.14-fix6: parse NMEA lon "DDDMM.MMMM" + hemi (E/W) into signed
// microdegrees. Same shape as parse_nmea_lat, but the integer part
// is 3 digits.
static bool parse_nmea_lon(const char *s, char hemi, int32_t *out) {
    if (!s) return false;
    if (hemi != 'E' && hemi != 'W') return false;
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9' ||
        s[2] < '0' || s[2] > '9') return false;

    const int ddd = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
    if (ddd < 0 || ddd > 179) return false;

    char *endp = NULL;
    const double minutes = strtod(s + 3, &endp);
    if (endp == s + 3) return false;
    if (minutes < 0.0 || minutes >= 60.0) return false;

    double deg = (double)ddd + minutes / 60.0;
    if (hemi == 'W') deg = -deg;

    *out = (int32_t)(deg * 1000000.0 + (deg >= 0.0 ? 0.5 : -0.5));
    return true;
}

// Parse a single $G?RMC sentence. Returns true if it's RMC and the
// time + status fields parsed cleanly (status itself may be A or V).
// On true: out->status == 'A' or 'V'; out->{hour,minute,second} valid
// when status='A'.
//
// Sentence format (NMEA 4.11, RMC fields by index):
//   0: $GxRMC     where x is N, P, A, etc.
//   1: HHMMSS.SS  UTC time
//   2: A | V      status (A=valid, V=void)
//   3: lat        DDMM.MMMM
//   4: N | S
//   5: lon        DDDMM.MMMM
//   6: E | W
//   7: speed knots
//   8: track deg
//   9: DDMMYY     UTC date
//   10: mag var
//   11: E | W
//   12+: mode ind, checksum
//
// We need fields 1, 2, 3, 4, 5, 6, and 9 (time, status, lat, N/S,
// lon, E/W, date). L7.14-fix6 grew the struct to carry position.
typedef struct {
    char    status;        // 'A' or 'V' (or 0 on parse failure)
    uint8_t hour, minute, second;
    uint8_t day, month;
    uint8_t year_yy;       // 2-digit; full year = 2000 + year_yy
    // L7.14-fix6: position from this sentence (only set when status='A'
    // typically — a void RMC usually has empty lat/lon fields). If
    // the GPS sends a status='V' but with stale position data, we
    // still parse it; the caller decides whether to trust it based
    // on the status field.
    bool    pos_valid;
    int32_t lat_microdeg;
    int32_t lon_microdeg;
} rmc_t;

static bool parse_rmc(const char *line, rmc_t *out) {
    if (!line || !out) return false;
    memset(out, 0, sizeof(*out));

    // Header form: $G[PNLA]RMC,  → at least 7 chars before the first
    // field separator. Tolerate any talker prefix the M10Q might emit
    // (GP for GPS-only, GN for multi-GNSS, etc).
    if (line[0] != '$' || line[1] != 'G' ||
        line[3] != 'R' || line[4] != 'M' || line[5] != 'C' ||
        line[6] != ',') {
        return false;
    }

    // Walk fields. Empty fields are valid in RMC (e.g. lat=empty when
    // status=V on cold start).
    const char *p = line + 7;  // first byte of field 1

    // Field 1: time HHMMSS.SS
    if (strlen(p) < 6) return false;
    int hh = two_digits(p);
    int mm = two_digits(p + 2);
    int ss = two_digits(p + 4);
    if (hh < 0 || mm < 0 || ss < 0 ||
        hh > 23 || mm > 59 || ss > 59) {
        return false;
    }

    // Advance to field 2.
    const char *comma = strchr(p, ',');
    if (!comma) return false;
    p = comma + 1;

    // Field 2: status. Single char A or V, then comma.
    if (*p == '\0') return false;
    char status_char = *p;
    if (status_char != 'A' && status_char != 'V') return false;

    // L7.14-fix6: walk forward to field 9 (date), capturing fields
    // 3-6 (lat, N/S, lon, E/W) along the way. Seven hops from field 2,
    // hop count unchanged from L7.14-fix1's verified field-skip logic:
    //   iter 0 → field 3 (lat string "DDMM.MMMM" or empty)
    //   iter 1 → field 4 (N/S, single char, or empty)
    //   iter 2 → field 5 (lon string "DDDMM.MMMM" or empty)
    //   iter 3 → field 6 (E/W, single char, or empty)
    //   iter 4 → field 7 (speed, ignored)
    //   iter 5 → field 8 (track, ignored)
    //   iter 6 → field 9 (date) ✓
    const char *lat_str  = NULL;
    const char *lon_str  = NULL;
    char        lat_hemi = 0;
    char        lon_hemi = 0;
    for (int hop = 0; hop < 7; ++hop) {
        comma = strchr(p, ',');
        if (!comma) return false;
        p = comma + 1;
        switch (hop) {
            case 0: lat_str  = p;  break;
            case 1: lat_hemi = *p; break;
            case 2: lon_str  = p;  break;
            case 3: lon_hemi = *p; break;
            default: break;
        }
    }

    // Field 9: date DDMMYY (may be empty if status=V on cold start).
    int dd = -1, mo = -1, yy = -1;
    if (p[0] != '\0' && p[0] != ',' && p[0] != '*') {
        if (strlen(p) < 6) {
            // Date present but short — treat as parse error.
            return false;
        }
        dd = two_digits(p);
        mo = two_digits(p + 2);
        yy = two_digits(p + 4);
        if (dd < 1 || dd > 31 || mo < 1 || mo > 12 || yy < 0) {
            return false;
        }
    }

    out->status  = status_char;
    out->hour    = (uint8_t)hh;
    out->minute  = (uint8_t)mm;
    out->second  = (uint8_t)ss;
    out->day     = (dd > 0) ? (uint8_t)dd : 0;
    out->month   = (mo > 0) ? (uint8_t)mo : 0;
    out->year_yy = (yy >= 0) ? (uint8_t)yy : 0;

    // L7.14-fix6: try to parse position from the captured fields.
    // Both hemisphere chars must be valid for us to consider the
    // position parseable. When the GPS has no fix it usually leaves
    // fields 3-6 empty, in which case lat_hemi/lon_hemi will be ','
    // or '\0' and parse_nmea_* will reject — pos_valid stays false.
    if ((lat_hemi == 'N' || lat_hemi == 'S') &&
        (lon_hemi == 'E' || lon_hemi == 'W')) {
        int32_t lat_md = 0, lon_md = 0;
        if (parse_nmea_lat(lat_str, lat_hemi, &lat_md) &&
            parse_nmea_lon(lon_str, lon_hemi, &lon_md)) {
            out->pos_valid    = true;
            out->lat_microdeg = lat_md;
            out->lon_microdeg = lon_md;
        }
    }

    return true;
}

// Dispatch one complete line (NUL-terminated, no '\r' or '\n').
// Updates state atomics and triggers re-sync when due.
static void handle_line(const char *line) {
    if (!line || line[0] == '\0') return;

    if (!nmea_checksum_ok(line)) {
        atomic_fetch_add(&s_parse_errors, 1);
        return;
    }

    rmc_t rmc;
    if (!parse_rmc(line, &rmc)) {
        // Not RMC — silently ignored (GGA, GSA, GSV, etc.).
        return;
    }

    atomic_fetch_add(&s_total_rmc, 1);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    atomic_store(&s_last_rmc_us, now_us);

    if (rmc.status != 'A') {
        // Module is alive and emitting sentences, but no valid fix yet.
        if (atomic_load(&s_status) == NANOJS8_GPS_NO_FIX) {
            atomic_store(&s_status, NANOJS8_GPS_SEARCHING);
        }
        return;
    }

    // Valid fix.
    atomic_fetch_add(&s_valid_fixes, 1);
    atomic_store(&s_last_fix_us, now_us);
    atomic_store(&s_last_fix_h, rmc.hour);
    atomic_store(&s_last_fix_m, rmc.minute);
    atomic_store(&s_last_fix_s, rmc.second);
    atomic_store(&s_status, NANOJS8_GPS_LOCKED);

    // L7.14-fix6: latch position from this fix. Once pos_valid is
    // true it stays true; even a subsequent void RMC (which omits
    // lat/lon) leaves the last good position visible to MYLOC. The
    // operator can still see status flip to SEARCHING via the GPS
    // row, but their last-known coords remain available.
    if (rmc.pos_valid) {
        atomic_store(&s_last_fix_lat_microdeg, rmc.lat_microdeg);
        atomic_store(&s_last_fix_lon_microdeg, rmc.lon_microdeg);
        atomic_store(&s_last_fix_pos_valid, true);
    }

    // Re-sync policy: first ever sync (s_last_sync_us == 0) → always.
    // Subsequent: only if RESYNC_INTERVAL_US elapsed.
    const uint64_t last_sync = atomic_load(&s_last_sync_us);
    if (last_sync == 0 || (now_us - last_sync) >= RESYNC_INTERVAL_US) {
        // Per project policy: GPS always wins, regardless of any
        // manual entry. set_utc_from_gps unconditionally overwrites.
        (void)nanojs8_time_set_utc_from_gps(rmc.hour, rmc.minute, rmc.second);
        atomic_store(&s_last_sync_us, now_us);
    }
}

// ── Reader task ───────────────────────────────────────────────────────

#ifdef CONFIG_NANOJS8_GPS_ENABLED

static char s_line_buf[LINE_BUF_MAX];
static size_t s_line_len = 0;

static void gps_reader_task(void *arg) {
    (void)arg;
    uint8_t chunk[RX_READ_CHUNK];

    for (;;) {
        const int n = uart_read_bytes(GPS_UART_NUM, chunk, sizeof(chunk),
                                      pdMS_TO_TICKS(RX_READ_WAIT_MS));
        const uint64_t now_us = (uint64_t)esp_timer_get_time();

        if (n > 0) {
            atomic_store(&s_last_byte_us, now_us);
            // If we were in NO_FIX state (no bytes seen yet), flip to
            // SEARCHING. A subsequent RMC with status=A will flip us
            // to LOCKED.
            if (atomic_load(&s_status) == NANOJS8_GPS_NO_FIX) {
                atomic_store(&s_status, NANOJS8_GPS_SEARCHING);
            }

            for (int i = 0; i < n; ++i) {
                const uint8_t b = chunk[i];
                if (b == '\r') continue;          // ignore CR
                if (b == '\n') {
                    // End of line — terminate and dispatch.
                    if (s_line_len > 0 && s_line_len < LINE_BUF_MAX) {
                        s_line_buf[s_line_len] = '\0';
                        handle_line(s_line_buf);
                    }
                    s_line_len = 0;
                    continue;
                }
                if (s_line_len < LINE_BUF_MAX - 1) {
                    s_line_buf[s_line_len++] = (char)b;
                } else {
                    // Overflow — discard the rest of this line.
                    s_line_len = LINE_BUF_MAX - 1;
                    atomic_fetch_add(&s_parse_errors, 1);
                }
            }
        }

        // Stale-fix timeout (L7.14-fix2): if we were LOCKED but
        // haven't seen a valid RMC in STALE_FIX_US, downgrade to
        // SEARCHING. Catches the "GPS lost satellite lock and is
        // sending void RMCs" case typical of indoor / weak-signal
        // operation. The time-component anchor is unaffected
        // (crystal-driven, ~50 ppm drift) so JS8 keeps operating;
        // the operator just sees the GPS row flip green→yellow.
        const uint64_t last_fix = atomic_load(&s_last_fix_us);
        if (atomic_load(&s_status) == NANOJS8_GPS_LOCKED &&
            last_fix != 0 && (now_us - last_fix) > STALE_FIX_US) {
            atomic_store(&s_status, NANOJS8_GPS_SEARCHING);
        }

        // Status timeout: if no bytes for STALE_BYTES_US, fall back to
        // NO_FIX (module unplugged / not responding). Done in the task
        // loop so the UI sees the transition without polling overhead.
        const uint64_t last_byte = atomic_load(&s_last_byte_us);
        if (last_byte != 0 && (now_us - last_byte) > STALE_BYTES_US) {
            atomic_store(&s_status, NANOJS8_GPS_NO_FIX);
        }
    }
}

#endif  // CONFIG_NANOJS8_GPS_ENABLED

// ── Public API ────────────────────────────────────────────────────────

esp_err_t nanojs8_gps_init(void) {
#ifndef CONFIG_NANOJS8_GPS_ENABLED
    // No-op when disabled. Sentinel state already in place from BSS.
    atomic_store(&s_status, NANOJS8_GPS_DISABLED);
    return ESP_OK;
#else
    if (atomic_load(&s_initialized)) {
        return ESP_ERR_INVALID_STATE;
    }

    // Configure UART1. RX-only effectively (we configure TX pin but
    // never write — keeps GPIO 43 idling high, which is the clean
    // state for the M10Q's RX line).
    const uart_config_t cfg = {
        .baud_rate  = GPS_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(GPS_UART_NUM, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(GPS_UART_NUM,
                       GPS_UART_TX_PIN, GPS_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    err = uart_driver_install(GPS_UART_NUM,
                              GPS_UART_RX_BUF,
                              /* tx_buf */ 0,
                              /* queue_size */ 0,
                              /* queue */ NULL,
                              /* intr_flags */ 0);
    if (err != ESP_OK) return err;

    // Spawn the reader task in PSRAM. Same pattern as tx_worker and
    // (since L7.13-fix3) self_test_task — avoids any chance of an
    // internal-RAM fragmentation failure mode.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        gps_reader_task, GPS_TASK_NAME, GPS_TASK_STACK,
        NULL, GPS_TASK_PRIORITY, NULL, GPS_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        uart_driver_delete(GPS_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_initialized, true);
    return ESP_OK;
#endif
}

void nanojs8_gps_get_snapshot(nanojs8_gps_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (!nanojs8_gps_is_enabled()) {
        out->status = NANOJS8_GPS_DISABLED;
        out->last_rmc_age_ms = UINT32_MAX;
        out->last_fix_age_ms = UINT32_MAX;
        return;
    }

    out->status = (nanojs8_gps_status_t)atomic_load(&s_status);
    out->hour   = atomic_load(&s_last_fix_h);
    out->minute = atomic_load(&s_last_fix_m);
    out->second = atomic_load(&s_last_fix_s);
    out->total_sentences = atomic_load(&s_total_rmc);
    out->valid_fixes     = atomic_load(&s_valid_fixes);
    out->parse_errors    = atomic_load(&s_parse_errors);
    // L7.14-fix6: last-known position (latches true on first valid
    // fix, never clears). UI/MYLOC caller is expected to gate on
    // status to decide how "fresh" this data is.
    out->pos_valid    = atomic_load(&s_last_fix_pos_valid);
    out->lat_microdeg = atomic_load(&s_last_fix_lat_microdeg);
    out->lon_microdeg = atomic_load(&s_last_fix_lon_microdeg);

    const uint64_t now_us  = (uint64_t)esp_timer_get_time();
    const uint64_t last_rmc = atomic_load(&s_last_rmc_us);
    const uint64_t last_fix = atomic_load(&s_last_fix_us);
    out->last_rmc_age_ms = (last_rmc == 0) ? UINT32_MAX
                          : (uint32_t)((now_us - last_rmc) / 1000ULL);
    out->last_fix_age_ms = (last_fix == 0) ? UINT32_MAX
                          : (uint32_t)((now_us - last_fix) / 1000ULL);
}

// L7.14-fix6: format last-known position for COMPOSE's MYLOC verb.
// Accepts LOCKED (fresh fix) or SEARCHING (we had a fix this session
// but recently lost it — coords are at most ~30s stale, still useful
// for "broadcast my location"). Refuses NO_FIX/DISABLED.
//
// Format: "DD.DDDD,DDD.DDDD" — signed decimal degrees with 4 places.
// 4 decimals = ~11 m precision, way better than JS8 message round-trip
// guarantees and tight enough that the operator can read it back.
bool nanojs8_gps_format_position(char *buf, size_t buf_n) {
    if (!buf || buf_n == 0) return false;
    buf[0] = '\0';

    nanojs8_gps_snapshot_t snap;
    nanojs8_gps_get_snapshot(&snap);

    if (snap.status != NANOJS8_GPS_LOCKED &&
        snap.status != NANOJS8_GPS_SEARCHING) {
        return false;
    }
    if (!snap.pos_valid) return false;

    const double lat = (double)snap.lat_microdeg * 1e-6;
    const double lon = (double)snap.lon_microdeg * 1e-6;
    const int n = snprintf(buf, buf_n, "%.4f,%.4f", lat, lon);
    if (n < 0 || (size_t)n >= buf_n) {
        // Truncation or encoding error — treat as failure.
        buf[0] = '\0';
        return false;
    }
    return true;
}

// ── Self-test (always compiled in) ────────────────────────────────────

bool nanojs8_gps_self_test(void) {
    ESP_LOGI(TAG, "Self-test starting (NMEA parser, no UART)");

    // Subtest 1: known-good RMC, computed XOR matches stated checksum.
    // Sentence is derived from the u-blox NMEA reference but uses
    // modern formatting (decimal-seconds time, 21st-century date).
    // Checksum 49 was verified offline by XOR'ing all bytes between
    // '$' and '*'; do NOT change this string without recomputing.
    const char *good = "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230322,003.1,W*49";
    if (!nmea_checksum_ok(good)) {
        ESP_LOGE(TAG, "Self-test 1 FAIL: known-good checksum rejected");
        return false;
    }
    rmc_t r;
    if (!parse_rmc(good, &r)) {
        ESP_LOGE(TAG, "Self-test 1 FAIL: known-good RMC parse failed");
        return false;
    }
    if (r.status != 'A' || r.hour != 12 || r.minute != 35 || r.second != 19 ||
        r.day != 23 || r.month != 3 || r.year_yy != 22) {
        ESP_LOGE(TAG,
            "Self-test 1 FAIL: parsed fields wrong "
            "(status=%c h=%u m=%u s=%u  d=%u mo=%u yy=%u)",
            r.status, r.hour, r.minute, r.second, r.day, r.month, r.year_yy);
        return false;
    }
    // L7.14-fix6: verify lat/lon extraction. Good sentence has
    //   lat 4807.038 N → 48 + 7.038/60 = 48.1173 deg     → 48117300 microdeg
    //   lon 01131.000 E → 11 + 31.000/60 = 11.51666... deg → 11516667 microdeg
    // Allow ±2 microdeg tolerance for floating-point rounding.
    if (!r.pos_valid) {
        ESP_LOGE(TAG, "Self-test 1 FAIL: pos_valid not set on good RMC");
        return false;
    }
    {
        const int32_t expected_lat = 48117300;
        const int32_t expected_lon = 11516667;
        const int32_t lat_err = (r.lat_microdeg > expected_lat)
                              ? (r.lat_microdeg - expected_lat)
                              : (expected_lat - r.lat_microdeg);
        const int32_t lon_err = (r.lon_microdeg > expected_lon)
                              ? (r.lon_microdeg - expected_lon)
                              : (expected_lon - r.lon_microdeg);
        if (lat_err > 2 || lon_err > 2) {
            ESP_LOGE(TAG,
                "Self-test 1 FAIL: lat/lon wrong "
                "(lat=%ld expected=%ld; lon=%ld expected=%ld)",
                (long)r.lat_microdeg, (long)expected_lat,
                (long)r.lon_microdeg, (long)expected_lon);
            return false;
        }
    }
    ESP_LOGI(TAG, "Self-test 1 PASS: good RMC parsed (12:35:19 UTC, 23-Mar-22, "
             "lat=%ld µdeg, lon=%ld µdeg)",
             (long)r.lat_microdeg, (long)r.lon_microdeg);

    // Subtest 2: bit-flipped checksum should be rejected. We mangle
    // the last hex digit of the *49 checksum from subtest 1 (49 → 4B),
    // which is definitely not the actual XOR result, so the validator
    // must reject it.
    char bad_cs[128];
    snprintf(bad_cs, sizeof(bad_cs), "%s", good);
    bad_cs[strlen(bad_cs) - 1] = 'B';  // 49 → 4B
    if (nmea_checksum_ok(bad_cs)) {
        ESP_LOGE(TAG, "Self-test 2 FAIL: flipped-checksum line accepted");
        return false;
    }
    ESP_LOGI(TAG, "Self-test 2 PASS: bad-checksum rejected");

    // Subtest 3: void status (V) — parsed but not marked valid.
    const char *void_rmc =
        "$GNRMC,000000.00,V,,,,,,,,,*7A";
    // checksum for that exact string is computed below; first verify it.
    // Computed XOR: G(47) N(4E) R(52) M(4D) C(43) ,(2C) 0(30) 0(30) 0(30) 0(30)
    // 0(30) 0(30) .(2E) 0(30) 0(30) ,(2C) V(56) ,(2C) ,(2C) ,(2C) ,(2C) ,(2C)
    // ,(2C) ,(2C) ,(2C) ,(2C) ,(2C) — result depends on exact comma count
    // and will be confirmed at runtime via the checksum check below.
    // We don't hard-code the expected XOR — instead recompute and patch.
    char fixed[64];
    snprintf(fixed, sizeof(fixed), "%s", void_rmc);
    // If our hard-coded checksum is wrong, recompute and rewrite the
    // last two hex digits so the test focuses on parser, not on
    // string-typing accuracy.
    {
        char *star = strchr(fixed, '*');
        if (star) {
            const uint8_t cs = nmea_compute_checksum(fixed);
            snprintf(star + 1, 3, "%02X", cs);
        }
    }
    if (!nmea_checksum_ok(fixed) || !parse_rmc(fixed, &r) ||
        r.status != 'V' || r.hour != 0 || r.minute != 0 || r.second != 0) {
        ESP_LOGE(TAG, "Self-test 3 FAIL: void RMC parse — status=%c h=%u",
                 r.status, r.hour);
        return false;
    }
    ESP_LOGI(TAG, "Self-test 3 PASS: void-status RMC handled");

    // Subtest 4: non-RMC sentence (GGA) → parse_rmc returns false.
    const char *gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    if (parse_rmc(gga, &r)) {
        ESP_LOGE(TAG, "Self-test 4 FAIL: GGA accepted as RMC");
        return false;
    }
    ESP_LOGI(TAG, "Self-test 4 PASS: non-RMC ignored");

    // Subtest 5: malformed inputs (no $, no *, empty) don't crash.
    (void)nmea_checksum_ok("");
    (void)nmea_checksum_ok("not a sentence");
    (void)nmea_checksum_ok("$truncated");
    (void)parse_rmc("$GPRMC,", &r);
    (void)parse_rmc(NULL, &r);
    ESP_LOGI(TAG, "Self-test 5 PASS: malformed inputs handled safely");

    ESP_LOGI(TAG, "Self-test: ALL PASS");
    return true;
}
