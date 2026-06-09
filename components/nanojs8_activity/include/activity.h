/*
 * activity.h — L7.9 in-memory station + traffic store
 * ====================================================
 * Two tables built from successful JS8 decodes:
 *
 *   HEARD — every station whose transmission we successfully decoded.
 *           Keyed by callsign. New decodes refresh last_seen / count;
 *           when full, oldest entry is evicted.
 *
 *   DIRECTED — every protocol-level verb exchange we decoded
 *           (HEARTBEAT, SNR replies, ACK, MSG announcements,
 *           CQ, QUERY MSGS, etc). Ring buffer, newest replaces
 *           oldest. NOT a free-text data store — data-frame
 *           bodies live with the multi-frame assembler.
 *
 * Threading
 * ---------
 * Writer: js8_sync task (one call per successful decode).
 * Reader: UI task (HEARD/DIRECTED screens take snapshots).
 * Both paths take a single static FreeRTOS mutex.
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

// ─── Sizes ───────────────────────────────────────────────────────────

/// Maximum stations kept in the HEARD table. 32 covers a busy band-window
/// (15 mins of 7.078 typically sees 10-20 unique stations).
#define NANOJS8_ACTIVITY_HEARD_MAX     32

/// Maximum entries in the DIRECTED ring buffer. 64 is roughly 15-20 minutes
/// of moderate band activity at one verb per slot.
#define NANOJS8_ACTIVITY_DIRECTED_MAX  64

/// Fixed string limits — match nanojs8_js8_codec sizes.
#define NANOJS8_ACTIVITY_CALL_MAX   16
#define NANOJS8_ACTIVITY_GRID_MAX   8
#define NANOJS8_ACTIVITY_VERB_MAX   24
// L7.11f-fix2d: was 32. Bumped to fit MicroJS8-style multi-frame
// payloads (8 frames × 12 chars/frame = 96 chars wire body) without
// the 31-char truncation seen on the COMPOSE→DIRECTED path in fix2c
// on-air testing. Cost: 64 DIRECTED entries × +64 B = +4 KB static.
#define NANOJS8_ACTIVITY_BODY_MAX   96

/// Direction tag for DIRECTED entries.
typedef enum {
    NANOJS8_ACTIVITY_DIR_IN  = 0,   // received from another station
    NANOJS8_ACTIVITY_DIR_OUT = 1,   // we transmitted (reserved for TX, L8+)
} nanojs8_activity_dir_t;

// ─── Records ─────────────────────────────────────────────────────────

/**
 * One row in the HEARD table.
 *
 * `at_boot_s` is monotonic seconds-since-boot (esp_timer-based) — the
 * UI computes "age" as `now_boot_s - at_boot_s`. This is independent of
 * the operator's UTC clock so age remains correct even if UTC drifts
 * or is re-entered mid-session.
 *
 * `utc_seconds_today` is captured at write time for display purposes
 * (HH:MM:SS in the table); 0xFFFFFFFF if UTC was not set when we heard
 * the station.
 *
 * Grid is empty if we have not (yet) decoded a heartbeat from that
 * station. distance_mi / bearing_deg are 0 / -1 respectively until a
 * grid arrives.
 */
typedef struct {
    char     callsign[NANOJS8_ACTIVITY_CALL_MAX];
    char     grid[NANOJS8_ACTIVITY_GRID_MAX];
    uint32_t at_boot_s;            // monotonic, for "age" calculation
    uint32_t utc_seconds_today;    // 0xFFFFFFFF if UTC unset
    float    audio_freq_hz;        // last observed
    uint16_t frame_count;          // how many times we've heard them
    int16_t  last_score;           // raw LDPC sync score (not radio SNR)
    int8_t   last_snr_db;          // L7.13: real SNR in dB, NANOJS8_ACTIVITY_SNR_NA if not yet computed
    int16_t  bearing_deg;          // 0..359, -1 = unknown (no grid yet)
    float    distance_mi;          // 0 if no grid yet
} nanojs8_activity_heard_t;

// L7.13: sentinel for "SNR not yet computed". -128 is outside any
// physically meaningful JS8 SNR range (real values clamp to [-60,+30]),
// so unambiguous. Matches the spirit of NANOJS8_MAILBOX_SNR_NA (127)
// in mailbox.h, just chosen on the opposite end of the int8_t range
// so the two values can't collide if a single field is ever used to
// store both kinds of "not applicable".
#define NANOJS8_ACTIVITY_SNR_NA  ((int8_t)-128)

/**
 * One row in the DIRECTED ring buffer.
 *
 * `verb` is the protocol-layer command ("HEARTBEAT", "@HB HEARTBEAT",
 * "SNR", "SNR?", "ACK", "MSG", "@ALLCALL CQ", etc.). `body` carries the
 * numeric argument or short payload ("-12", "EM44XX", etc.). For
 * heartbeats, to_call is empty (heartbeats are broadcast).
 */
typedef struct {
    uint32_t at_boot_s;
    uint32_t utc_seconds_today;
    char     from_call[NANOJS8_ACTIVITY_CALL_MAX];
    char     to_call[NANOJS8_ACTIVITY_CALL_MAX];     // empty if broadcast
    char     verb[NANOJS8_ACTIVITY_VERB_MAX];
    char     body[NANOJS8_ACTIVITY_BODY_MAX];
    int16_t  score;
    float    freq_hz;
    uint8_t  direction;            // nanojs8_activity_dir_t
} nanojs8_activity_directed_t;

// ─── Lifecycle ───────────────────────────────────────────────────────

/// Initialize state + mutex. Idempotent; safe to call once at boot.
/// Must be called before any record / snapshot call.
esp_err_t nanojs8_activity_init(void);

/// Drop all stored entries. Used by the UI's eventual "reset traffic"
/// hotkey; not invoked automatically.
void nanojs8_activity_clear(void);

// ─── Write path (js8_sync task) ──────────────────────────────────────

/**
 * Single ingest call invoked after a successful decode + protocol parse.
 * Internally categorises the frame and updates the right table(s):
 *
 *   - `from_call` (if non-empty) → upsert into HEARD
 *   - `to_call` non-empty (directed) → append DIRECTED entry
 *   - heartbeats → append DIRECTED entry (with empty to_call)
 *   - data-only frames (from_call empty) → not stored here; multi-frame
 *     assembly is handled in js8_codec
 *
 * Safe to call with any combination of fields empty — the function
 * skips paths that would store nothing.
 */
void nanojs8_activity_record_decode(
    const char *from_call,
    const char *to_call,
    const char *verb,
    const char *body,
    const char *grid,
    int         score,
    int8_t      snr_db,         // L7.13: real radio SNR; NANOJS8_ACTIVITY_SNR_NA if unavailable
    float       audio_freq_hz);

// ─── Write path (UI / TX-side) ───────────────────────────────────────

/**
 * L7.11f-fix2c: record an outbound directed-log entry (us transmitting).
 * Used by COMPOSE SEND and ALLCALL row actions so the operator sees
 * their own messages interleaved with received traffic on the DIRECTED
 * screen, matching MicroJS8 behavior.
 *
 * `from_call` is auto-populated from the active config's callsign.
 * `direction` is set to NANOJS8_ACTIVITY_DIR_OUT. `score` and `freq_hz`
 * are zero — they have no meaning on the TX side (the operator already
 * knows their own carrier and there's no measured SNR).
 *
 * `to_call` examples: "KD8PGB" (directed reply), "@HB" (heartbeat
 * broadcast), "@ALLCALL" (QUERY MSGS / CQ).
 *
 * `verb` is uppercase by convention: "HEARTBEAT", "QUERY", "CQ", "SNR",
 * "ACK", or in the COMPOSE case whatever token followed `to_call` in
 * the wire string.
 *
 * `body` may be empty for verb-only forms like ACK; otherwise carries
 * the grid, numeric argument, or remainder of the wire string.
 *
 * No-op (with a WARN log) if no callsign is configured yet — we'd
 * be writing a from_call="" row that the DIRECTED filter would drop.
 * Thread-safe; serialised on the same mutex as record_decode.
 */
void nanojs8_activity_record_out(
    const char *to_call,
    const char *verb,
    const char *body);

/**
 * L7.11f-fix2f: replace the body of the most recent matching IN-direction
 * DIRECTED entry with the assembler's joined multi-frame text.
 *
 * Background: when a remote station sends a multi-frame directed message
 * (e.g. "WD5EED: W5DMH GRID" header followed one slot later by "EM44"
 * data), the per-frame record_decode call only stores the header frame
 * (from/to/verb present) — the data continuation has empty from/verb
 * fields and is dropped. The msg_assembler concatenates the chunks into
 * a single text string and emits it as kind=PARTIAL_TAIL or kind=COMPLETE,
 * but until now nothing fed that joined body back to the operator-facing
 * activity store.
 *
 * Match rule: walk DIRECTED newest-first; the first DIR_IN entry whose
 * audio_freq_hz is within ±25 Hz of the given freq AND whose at_boot_s
 * is within the last 60 s wins. ±25 Hz matches the msg_assembler's own
 * freq tolerance; 60 s is well under the assembler's 90 s idle-GC window
 * yet long enough to bridge typical 2–8-frame messages (~30–120 s span).
 *
 * On match, the entry's body field is REPLACED with `body_text` (truncated
 * to NANOJS8_ACTIVITY_BODY_MAX-1). The assembler already concatenates all
 * received chunks, so its output is the full body — replace is correct,
 * append would double-write.
 *
 * No-op if no match (we may have joined mid-stream and never saw the
 * header). Thread-safe; serialised on the same mutex as record_decode.
 */
void nanojs8_activity_set_body_continuation(
    float       audio_freq_hz,
    const char *body_text);

// ─── Read path (UI task) ─────────────────────────────────────────────

/**
 * Copy up to `max` HEARD rows into `out`, newest first (by at_boot_s).
 * Returns the number actually copied. Thread-safe — uses an internal
 * mutex; concurrent record_decode() calls are serialised.
 */
uint32_t nanojs8_activity_snapshot_heard(
    nanojs8_activity_heard_t *out, uint32_t max);

/**
 * Copy up to `max` DIRECTED rows into `out`, newest first.
 * Returns the number actually copied.
 */
uint32_t nanojs8_activity_snapshot_directed(
    nanojs8_activity_directed_t *out, uint32_t max);

/// Total HEARD entries currently stored (≤ NANOJS8_ACTIVITY_HEARD_MAX).
uint32_t nanojs8_activity_heard_count(void);

/// Total DIRECTED entries currently stored (≤ NANOJS8_ACTIVITY_DIRECTED_MAX).
uint32_t nanojs8_activity_directed_count(void);

// ─── Filter helper ───────────────────────────────────────────────────

/**
 * Test whether a decoded frame's `to_call` is "for us" — used by the
 * DIRECTED screen to filter the ring-buffer snapshot down to entries
 * the operator actually cares about.
 *
 * Matches MicroJS8's behavior (app.py ~line 2017):
 *   - case-insensitive equality with `my_call`
 *   - OR: starts with '@', not @ALLCALL/@HB, AND appears in `groups_csv`
 *
 * `groups_csv` is the comma-separated list as stored by SETUP
 * (e.g. "@GHOSTNET,@AMRRON"). Whitespace around tokens is trimmed.
 * NULL or empty groups_csv → group check skipped.
 *
 * Empty `to_call` → false (heartbeats, ALLCALL broadcasts).
 * `to_call` of "@ALLCALL" / "@HB" → false (broadcasts are NOT
 * personal traffic per the MicroJS8 convention).
 */
bool nanojs8_activity_is_for_me(const char *to_call,
                                 const char *my_call,
                                 const char *groups_csv);

#ifdef __cplusplus
}
#endif
