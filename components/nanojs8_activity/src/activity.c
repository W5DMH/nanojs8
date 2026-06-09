/*
 * activity.c — L7.9 station + traffic store (impl)
 * =================================================
 * Two static tables guarded by one FreeRTOS mutex. Designed for one
 * writer (js8_sync) and one reader (UI). Snapshots return copies so
 * the UI never holds the lock while painting.
 *
 * License: GPL-3.0
 */

#include "activity.h"
#include "grid_math.h"

#include <string.h>
#include <strings.h>      // L7.11f-fix2d: strcasecmp for self-decode skip
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"               // operator's grid
#include "time_source.h"          // nanojs8_time_get_utc / seconds_today

static const char *TAG = "activity";

// ── Internal state ───────────────────────────────────────────────────

// Heard table — unordered storage, sorted on snapshot.
static nanojs8_activity_heard_t    s_heard[NANOJS8_ACTIVITY_HEARD_MAX];
static uint32_t                    s_heard_count = 0;

// Directed ring buffer — `s_dir_count` clamped to MAX once filled;
// `s_dir_head` is the next write position; oldest entry is at
// (s_dir_head - s_dir_count + MAX) % MAX.
static nanojs8_activity_directed_t s_directed[NANOJS8_ACTIVITY_DIRECTED_MAX];
static uint32_t                    s_dir_head  = 0;
static uint32_t                    s_dir_count = 0;

static SemaphoreHandle_t           s_mutex = NULL;
static bool                        s_inited = false;

// ── Helpers ──────────────────────────────────────────────────────────

static inline uint32_t now_boot_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

/// Safe copy: NUL-terminates, truncates at cap-1. NULL src treated as "".
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    const size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/// Find a HEARD entry by callsign (case-sensitive — JS8 callsigns are
/// canonical-uppercase). Returns NULL if not found.
static nanojs8_activity_heard_t *find_heard(const char *call) {
    if (!call || !call[0]) return NULL;
    for (uint32_t i = 0; i < s_heard_count; ++i) {
        if (strncmp(s_heard[i].callsign, call,
                    NANOJS8_ACTIVITY_CALL_MAX) == 0) {
            return &s_heard[i];
        }
    }
    return NULL;
}

/// Allocate a slot for a new HEARD entry. If table is full, evict the
/// oldest by at_boot_s.
static nanojs8_activity_heard_t *alloc_heard(void) {
    if (s_heard_count < NANOJS8_ACTIVITY_HEARD_MAX) {
        return &s_heard[s_heard_count++];
    }
    // Evict oldest.
    uint32_t oldest_idx = 0;
    uint32_t oldest_at  = s_heard[0].at_boot_s;
    for (uint32_t i = 1; i < s_heard_count; ++i) {
        if (s_heard[i].at_boot_s < oldest_at) {
            oldest_at  = s_heard[i].at_boot_s;
            oldest_idx = i;
        }
    }
    ESP_LOGD(TAG, "evicting HEARD[%u] %s (age %us)",
             (unsigned)oldest_idx, s_heard[oldest_idx].callsign,
             (unsigned)(now_boot_s() - oldest_at));
    memset(&s_heard[oldest_idx], 0, sizeof(s_heard[0]));
    s_heard[oldest_idx].bearing_deg = -1;   // "unknown" sentinel
    s_heard[oldest_idx].last_snr_db = NANOJS8_ACTIVITY_SNR_NA;
    return &s_heard[oldest_idx];
}

/// Compute distance / bearing from operator's grid to `their_grid`.
/// Fills with (0, -1) if either grid is unparseable.
static void compute_geom(const char *their_grid,
                          float *distance_mi, int16_t *bearing_deg) {
    *distance_mi = 0.0f;
    *bearing_deg = -1;

    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (!cfg) return;
    if (cfg->grid[0] == '\0' || their_grid[0] == '\0') return;

    double miles = 0.0;
    int    brg   = 0;
    if (nanojs8_grid_distance_bearing(cfg->grid, their_grid, &miles, &brg)) {
        *distance_mi = (float)miles;
        *bearing_deg = (int16_t)brg;
    }
}

// ── Public API ───────────────────────────────────────────────────────

esp_err_t nanojs8_activity_init(void) {
    if (s_inited) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    memset(s_heard,    0, sizeof(s_heard));
    memset(s_directed, 0, sizeof(s_directed));
    for (uint32_t i = 0; i < NANOJS8_ACTIVITY_HEARD_MAX; ++i) {
        s_heard[i].bearing_deg  = -1;
        s_heard[i].last_snr_db  = NANOJS8_ACTIVITY_SNR_NA;
    }
    s_heard_count = 0;
    s_dir_head    = 0;
    s_dir_count   = 0;
    s_inited      = true;

    ESP_LOGI(TAG, "Activity store ready: HEARD<=%u, DIRECTED<=%u, "
                  "static footprint=%u B",
             (unsigned)NANOJS8_ACTIVITY_HEARD_MAX,
             (unsigned)NANOJS8_ACTIVITY_DIRECTED_MAX,
             (unsigned)(sizeof(s_heard) + sizeof(s_directed)));
    return ESP_OK;
}

void nanojs8_activity_clear(void) {
    if (!s_inited) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    memset(s_heard,    0, sizeof(s_heard));
    memset(s_directed, 0, sizeof(s_directed));
    for (uint32_t i = 0; i < NANOJS8_ACTIVITY_HEARD_MAX; ++i) {
        s_heard[i].bearing_deg  = -1;
        s_heard[i].last_snr_db  = NANOJS8_ACTIVITY_SNR_NA;
    }
    s_heard_count = 0;
    s_dir_head    = 0;
    s_dir_count   = 0;
    xSemaphoreGive(s_mutex);
}

void nanojs8_activity_record_decode(
    const char *from_call,
    const char *to_call,
    const char *verb,
    const char *body,
    const char *grid,
    int         score,
    int8_t      snr_db,
    float       audio_freq_hz)
{
    if (!s_inited) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "record_decode: mutex timeout");
        return;
    }

    // L7.11f-fix2d: drop self-decodes. When DigiRig is in loopback
    // during TX (very common in normal operation — the host's audio
    // out IS the host's audio in), our own carrier decodes back via
    // the RX path with from_call == operator's callsign. Storing it
    // here pollutes HEARD with ourselves, DIRECTED IN ring with our
    // own messages (green inbound rows), and the ALL firehose. The
    // OUT side (recorded via nanojs8_activity_record_out from the
    // SEND code paths) is the only canonical operator-facing trail
    // for outbound traffic — self-decode is just a diagnostic in the
    // serial log, not a user-visible event. Active callsign is read
    // through nanojs8_config_get() (already a REQUIRES); cfg cache
    // is updated atomically by the config layer, no extra lock.
    if (from_call && from_call[0]) {
        const nanojs8_config_t *cfg = nanojs8_config_get();
        if (cfg && cfg->callsign[0]) {
            if (strcasecmp(from_call, cfg->callsign) == 0) {
                xSemaphoreGive(s_mutex);
                return;
            }
        }
    }

    const uint32_t now_b      = now_boot_s();
    const uint32_t now_utc_s  = nanojs8_time_is_set()
                                ? nanojs8_time_seconds_today()
                                : 0xFFFFFFFFu;

    // ── HEARD: upsert by from_call ──────────────────────────────────
    if (from_call && from_call[0]) {
        nanojs8_activity_heard_t *h = find_heard(from_call);
        if (!h) {
            h = alloc_heard();
            copy_str(h->callsign, sizeof(h->callsign), from_call);
            h->frame_count = 0;
        }
        h->at_boot_s         = now_b;
        h->utc_seconds_today = now_utc_s;
        h->audio_freq_hz     = audio_freq_hz;
        h->frame_count      += 1;
        h->last_score        = (int16_t)score;
        h->last_snr_db       = snr_db;    // L7.13: most-recent radio SNR

        // Grid: only update if we now have one and either we didn't
        // before, or it's longer (e.g. upgrading 4-char to 6-char).
        if (grid && grid[0]) {
            if (h->grid[0] == '\0' || strlen(grid) > strlen(h->grid)) {
                copy_str(h->grid, sizeof(h->grid), grid);
                compute_geom(h->grid, &h->distance_mi, &h->bearing_deg);
            }
        }
    }

    // ── DIRECTED: heartbeats and verb-carrying frames go in the log ──
    //
    // We log:
    //   - any frame with a non-empty `verb` (heartbeat, SNR, ACK, MSG,
    //     CQ, etc.) AND a known from_call
    // We skip:
    //   - frames without from_call (data-only chunks)
    //   - frames with empty verb (shouldn't happen from upstream parse)
    const bool log_directed =
        (from_call && from_call[0]) && (verb && verb[0]);

    if (log_directed) {
        nanojs8_activity_directed_t *d = &s_directed[s_dir_head];
        memset(d, 0, sizeof(*d));
        d->at_boot_s         = now_b;
        d->utc_seconds_today = now_utc_s;
        copy_str(d->from_call, sizeof(d->from_call), from_call);
        copy_str(d->to_call,   sizeof(d->to_call),   to_call);
        copy_str(d->verb,      sizeof(d->verb),      verb);
        copy_str(d->body,      sizeof(d->body),      body);
        d->score     = (int16_t)score;
        d->freq_hz   = audio_freq_hz;
        d->direction = NANOJS8_ACTIVITY_DIR_IN;

        s_dir_head = (s_dir_head + 1) % NANOJS8_ACTIVITY_DIRECTED_MAX;
        if (s_dir_count < NANOJS8_ACTIVITY_DIRECTED_MAX) s_dir_count++;
    }

    xSemaphoreGive(s_mutex);
}

// ─── L7.11f-fix2c: outbound TX-side record ──────────────────────────
//
// Records `we transmitted to ...` so DIRECTED shows our own messages
// interleaved with received traffic. See header for full semantics.
//
// Why this path is separate from record_decode():
//   - direction is hard-wired to DIR_OUT
//   - from_call is auto-pulled from config (vs supplied by caller)
//   - no SNR / no audio_freq_hz (meaningless on the TX side)
//   - no HEARD-table update (we're not "hearing" ourselves)

void nanojs8_activity_record_out(
    const char *to_call,
    const char *verb,
    const char *body)
{
    if (!s_inited) return;
    if (!verb || !verb[0]) {
        ESP_LOGW(TAG, "record_out: empty verb — skipping log");
        return;
    }

    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (!cfg || cfg->callsign[0] == '\0') {
        ESP_LOGW(TAG, "record_out: no callsign configured — skipping "
                      "(would write from_call='' which DIRECTED filter "
                      "drops anyway)");
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "record_out: mutex timeout");
        return;
    }

    const uint32_t now_b     = now_boot_s();
    const uint32_t now_utc_s = nanojs8_time_is_set()
                               ? nanojs8_time_seconds_today()
                               : 0xFFFFFFFFu;

    nanojs8_activity_directed_t *d = &s_directed[s_dir_head];
    memset(d, 0, sizeof(*d));
    d->at_boot_s         = now_b;
    d->utc_seconds_today = now_utc_s;
    copy_str(d->from_call, sizeof(d->from_call), cfg->callsign);
    copy_str(d->to_call,   sizeof(d->to_call),   to_call);
    copy_str(d->verb,      sizeof(d->verb),      verb);
    copy_str(d->body,      sizeof(d->body),      body);
    d->score     = 0;
    d->freq_hz   = 0.0f;
    d->direction = NANOJS8_ACTIVITY_DIR_OUT;

    s_dir_head = (s_dir_head + 1) % NANOJS8_ACTIVITY_DIRECTED_MAX;
    if (s_dir_count < NANOJS8_ACTIVITY_DIRECTED_MAX) s_dir_count++;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "OUT  to='%s'  verb='%s'  body='%s'",
             to_call ? to_call : "", verb, body ? body : "");
}

// L7.11f-fix2f: feed multi-frame-assembled body text back into the most
// recent matching IN entry. See header for rationale + match rule.
void nanojs8_activity_set_body_continuation(
    float       audio_freq_hz,
    const char *body_text)
{
    if (!s_inited || !body_text || !body_text[0]) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "set_body_continuation: mutex timeout");
        return;
    }

    const uint32_t now_b = now_boot_s();
    const float    freq_tol_hz = 25.0f;
    const uint32_t age_max_s   = 60u;

    // Walk DIRECTED newest-first (same iteration pattern as snapshot).
    // The ring's head points one past the newest entry; (head - 1) wraps
    // to the newest. Stop at the first matching IN entry — older ones
    // would be earlier-headed messages that already had their body set.
    nanojs8_activity_directed_t *match = NULL;
    uint32_t idx = (s_dir_head + NANOJS8_ACTIVITY_DIRECTED_MAX - 1)
                   % NANOJS8_ACTIVITY_DIRECTED_MAX;
    for (uint32_t i = 0; i < s_dir_count; ++i) {
        nanojs8_activity_directed_t *d = &s_directed[idx];

        // IN-only: OUT entries are our own transmissions and never need
        // continuation appending (we already know the full body locally).
        if (d->direction == NANOJS8_ACTIVITY_DIR_IN) {
            const uint32_t age_s = now_b - d->at_boot_s;
            if (age_s <= age_max_s) {
                const float df = (audio_freq_hz > d->freq_hz)
                               ? (audio_freq_hz - d->freq_hz)
                               : (d->freq_hz - audio_freq_hz);
                if (df <= freq_tol_hz) {
                    match = d;
                    break;
                }
            } else {
                // Newest-first walk: once we cross the age window, no
                // older entry will match either. Bail early.
                break;
            }
        }

        idx = (idx + NANOJS8_ACTIVITY_DIRECTED_MAX - 1)
              % NANOJS8_ACTIVITY_DIRECTED_MAX;
    }

    if (match) {
        // Replace body with the assembler's full joined text. The
        // assembler already concatenated all received chunks, so this
        // is the canonical full payload — no append needed.
        copy_str(match->body, sizeof(match->body), body_text);
        ESP_LOGI(TAG, "Continuation: set body='%s' on entry from='%s' "
                      "to='%s' verb='%s' @ %.1f Hz",
                 match->body, match->from_call, match->to_call,
                 match->verb, (double)match->freq_hz);
    } else {
        ESP_LOGD(TAG, "Continuation '%s' @ %.1f Hz had no matching IN "
                      "entry within ±%.0fHz / %us — discarded",
                 body_text, (double)audio_freq_hz,
                 (double)freq_tol_hz, (unsigned)age_max_s);
    }

    xSemaphoreGive(s_mutex);
}

uint32_t nanojs8_activity_snapshot_heard(
    nanojs8_activity_heard_t *out, uint32_t max)
{
    if (!s_inited || !out || max == 0) return 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "snapshot_heard: mutex timeout");
        return 0;
    }

    // Copy all into out (clipped to max), then sort newest-first by
    // at_boot_s. Insertion sort — N ≤ 32, trivially fast.
    const uint32_t n = (s_heard_count < max) ? s_heard_count : max;
    for (uint32_t i = 0; i < n; ++i) out[i] = s_heard[i];

    for (uint32_t i = 1; i < n; ++i) {
        nanojs8_activity_heard_t tmp = out[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && out[j].at_boot_s < tmp.at_boot_s) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = tmp;
    }

    xSemaphoreGive(s_mutex);
    return n;
}

uint32_t nanojs8_activity_snapshot_directed(
    nanojs8_activity_directed_t *out, uint32_t max)
{
    if (!s_inited || !out || max == 0) return 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "snapshot_directed: mutex timeout");
        return 0;
    }

    const uint32_t n = (s_dir_count < max) ? s_dir_count : max;
    // Newest first: walk backwards from (head-1) wrapping modulo MAX.
    uint32_t idx = (s_dir_head + NANOJS8_ACTIVITY_DIRECTED_MAX - 1)
                   % NANOJS8_ACTIVITY_DIRECTED_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = s_directed[idx];
        idx = (idx + NANOJS8_ACTIVITY_DIRECTED_MAX - 1)
              % NANOJS8_ACTIVITY_DIRECTED_MAX;
    }

    xSemaphoreGive(s_mutex);
    return n;
}

uint32_t nanojs8_activity_heard_count(void) {
    // Read of single uint32 is atomic on ESP32-S3; no lock needed for stats.
    return s_heard_count;
}

uint32_t nanojs8_activity_directed_count(void) {
    return s_dir_count;
}

// ── Filter helper ────────────────────────────────────────────────────

/// Case-insensitive string equality. NULL-safe.
static bool ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        const int ca = (int)(unsigned char)*a;
        const int cb = (int)(unsigned char)*b;
        const int la = (ca >= 'a' && ca <= 'z') ? (ca - 32) : ca;
        const int lb = (cb >= 'a' && cb <= 'z') ? (cb - 32) : cb;
        if (la != lb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool nanojs8_activity_is_for_me(const char *to_call,
                                 const char *my_call,
                                 const char *groups_csv)
{
    // Empty to_call → not directed at anyone in particular.
    if (!to_call || to_call[0] == '\0') return false;

    // Reject broadcast pseudo-callsigns up front (matches MicroJS8).
    if (ieq(to_call, "@ALLCALL")) return false;
    if (ieq(to_call, "@HB"))      return false;

    // Personal match: to_call == my callsign (case-insensitive).
    if (my_call && my_call[0] && ieq(to_call, my_call)) return true;

    // Group match: walk comma-separated groups, case-insensitive per token.
    if (!groups_csv || groups_csv[0] == '\0')   return false;
    if (to_call[0] != '@')                      return false;  // not a group addr

    const char *p = groups_csv;
    while (*p) {
        // Skip leading whitespace + commas.
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;

        // Identify token end.
        const char *start = p;
        while (*p && *p != ',') ++p;
        const char *end = p;
        // Trim trailing whitespace.
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;

        // Compare token (start..end) with to_call.
        const char *a = to_call;
        const char *b = start;
        bool match = true;
        while (b < end && *a) {
            const int ca = (int)(unsigned char)*a;
            const int cb = (int)(unsigned char)*b;
            const int la = (ca >= 'a' && ca <= 'z') ? (ca - 32) : ca;
            const int lb = (cb >= 'a' && cb <= 'z') ? (cb - 32) : cb;
            if (la != lb) { match = false; break; }
            ++a; ++b;
        }
        if (match && b == end && *a == '\0') return true;
    }
    return false;
}
