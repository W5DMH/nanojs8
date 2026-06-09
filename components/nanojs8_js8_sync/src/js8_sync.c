/*
 * js8_sync.c — JS8 Normal-mode sync detection (L7.5)
 * =====================================================================
 * Architecture:
 *
 *   rx_audio (12 kHz int16, 60 s ring + slot snapshot)
 *       │
 *       │  nanojs8_rx_audio_snapshot_last_slot(snapshot, 180 000)
 *       ▼
 *   This task — runs on Core 1, priority 5:
 *       1. Wait for new slot (poll slots_fired counter @ 200 ms)
 *       2. Snapshot the slot's audio into PSRAM buffer
 *       3. monitor_reset()
 *       4. For each 1920-sample block (×93 in a 15 s slot):
 *            a. Convert int16 → float (in a small 7.7 KB chunk buffer)
 *            b. monitor_process(&mon, chunk) → updates waterfall
 *       5. ftx_find_candidates(&mon.wf, MAX_CAND, heap, MIN_SCORE)
 *       6. Log each candidate; update atomic stats
 *
 * Sync only — no LDPC, no message extraction. Layers 7.6/7.7 add those.
 *
 * License: GPL-3.0
 */

#include "js8_sync.h"

// rx_audio — slot snapshot source
#include "rx_audio.h"

// nanojs8_time — UTC-gating
#include "time_source.h"

// ft8_lib (with NanoJS8's JS8-Normal Costas swap in constants.c)
#include "monitor.h"
#include "decode.h"
#include "constants.h"

// L7.6: JS8 codec — LLR extraction + LDPC + CRC + message extraction
#include "js8_codec.h"
#include "js8_freetext.h" // L7.11g.7-fix6: body sanitization before wire pack
#include "activity.h"   // L7.9: HEARD + DIRECTED ingest
// L7.11g.4: RX MSG verb → INBOX + auto-ACK chain
#include "config.h"     // operator callsign + groups CSV for is_for_me()
#include "mailbox.h"    // nanojs8_mailbox_add_unread + SNR_NA sentinel
#include "tx_queue.h"   // 4-deep FIFO for queued short-form TX (ACK et al)
#include <strings.h>    // strcasecmp for own-callsign filter

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>      // snprintf (L7.6 decode logging)
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h" // L7.14-fix7: xTaskCreatePinnedToCoreWithCaps + vTaskDeleteWithCaps for PSRAM-backed stack
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <math.h>      // L7.13: log10f/powf/lroundf for compute_snr_db

static const char *TAG = "js8sync";

// ── Sync-detection configuration ─────────────────────────────────────────────

// 12 kHz audio matches our rx_audio decimator output and JS8 spec.
#define JS8_SYNC_SAMPLE_RATE   12000

// JS8 audio passband. Lower bound 200 Hz cuts rumble + DC; upper 3000 Hz
// matches a typical SSB receiver's 2.7-3.0 kHz audio bandwidth.
#define JS8_SYNC_F_MIN          200.0f
#define JS8_SYNC_F_MAX         3000.0f

// time_osr = 2 — analyze every half-symbol (vs once-per-symbol). Doubles
// timing resolution at modest cost. freq_osr = 2 — sub-bin frequency
// resolution; gives ~3.125 Hz spectral resolution. Both values match
// what Mini-FT8 used at L7.3 (proven to work).
#define JS8_SYNC_TIME_OSR        2
#define JS8_SYNC_FREQ_OSR        2

// Candidate-list size. ftx_find_candidates returns a min-heap; size
// here caps the total candidates tracked per slot. 30 is plenty — typical
// 7.078 MHz JS8 traffic shows 5-20 candidates per slot when active.
#define JS8_SYNC_MAX_CAND       30

// Minimum Costas correlation score to keep. Lower = more permissive
// (more false positives in heap). Mini-FT8 used 10 for FT8 RX at L7.3.
// We start with 10 too; tune after on-air results.
#define JS8_SYNC_MIN_SCORE      10

// ── Atomic stats — heartbeat reads without locking ───────────────────────────

static struct {
    _Atomic uint32_t slots_processed;
    _Atomic uint32_t last_slot_candidates;
    _Atomic uint32_t total_candidates;
    _Atomic int      last_slot_best_score;   // signed; can be 0
    _Atomic uint32_t last_slot_cpu_ms;
    _Atomic uint32_t stack_min_free;
    // L7.6: LDPC + CRC decode counters
    _Atomic uint32_t last_slot_attempts;
    _Atomic uint32_t last_slot_decodes;
    _Atomic uint32_t total_decodes;
} g_stats = {
    .stack_min_free = UINT32_MAX,
};

// ── Component-local state — only touched on the sync task ────────────────────

// Slot snapshot — 180 000 int16 = 360 KB. Allocated PSRAM at start.
static int16_t *s_snapshot = NULL;

// Per-block float conversion buffer. Block size = 1920 floats = 7.7 KB.
// Declared file-static so it doesn't burn stack each iteration. Live in
// .bss (DRAM static) which is fine — 7.7 KB out of ~300 KB free.
#define BLOCK_SIZE 1920u
static float s_chunk[BLOCK_SIZE];

// The monitor + candidate heap.
static monitor_t s_mon;
static ftx_candidate_t s_cand_heap[JS8_SYNC_MAX_CAND];

// Slot-fire watermark.
static uint32_t s_last_seen_slots = 0;

// ── L7.11g.4-fix1: pending MSG tracker ───────────────────────────────────────
//
// JS8 directed MSGs are multi-frame: frame 1 carries the directed
// header (FROM, TO, VERB=MSG, body empty for typical MSG); subsequent
// frames are FRAME_DATA chunks that the assembler joins into a full
// body. The sender transmits all frames in CONSECUTIVE slots without
// listening between — they only start listening for an ACK after the
// final data frame.
//
// Auto-ACK timing must therefore wait until the assembler emits
// PARTIAL_TAIL or COMPLETE at the matching freq. ACKing on the
// header frame would collide with the sender's continuation
// transmissions and waste both stations' airtime.
//
// State: 4 slots × ~40 B = ~160 B BSS. Freq tolerance ±25 Hz
// matches the assembler's own grouping window. Slots time out at
// 60 s — same as the assembler's idle GC — so an abandoned header
// without ever-arriving data frames doesn't permanently consume a
// slot.

#define MSG_PENDING_SLOTS         4
#define MSG_PENDING_FREQ_TOL_HZ   25
#define MSG_PENDING_TIMEOUT_US    (60LL * 1000LL * 1000LL)

typedef struct {
    bool     active;
    uint32_t freq_hz;       // audio Hz at decode time
    char     from_call[NANOJS8_MAILBOX_CALL_LEN];
    char     to_call  [NANOJS8_MAILBOX_CALL_LEN];
    int64_t  header_seen_us;
} msg_pending_t;

static msg_pending_t s_msg_pending[MSG_PENDING_SLOTS];

// freq tolerance helper (absolute diff under uint32 arithmetic).
static inline bool freq_within_tol(uint32_t a, uint32_t b, uint32_t tol)
{
    return (a > b) ? ((a - b) <= tol) : ((b - a) <= tol);
}

// L7.13 (fixed in L7.13-fix2): Real radio SNR from waterfall,
// computed at the 21 Costas sync-tone positions. The algorithm is a
// port of gfsk8's xsig/xbase ratio formula (gfsk8::JS8.cpp::tryDecode,
// ~line 1581) adapted to ft8_lib's waterfall layout.
//
//   JS8/FT8 Costas pattern: kFT8_Costas_pattern = {4,2,5,6,1,3,0}
//   Three sync blocks at symbol offsets 0..6, 36..42, 72..78
//   → 21 (symbol, expected_tone) pairs per frame
//
// For each Costas symbol we extract the magnitude at the expected
// tone bin (signal) and the seven other tone bins (noise), accumulate
// in linear units, then average and apply gfsk8's formula:
//
//   snr_dB = 10 * log10( (sig_avg - noise_avg) / noise_avg ) - 26.0
//
// Why -26 dB? The waterfall is filled with one entry per logical
// tone-spaced bin (see monitor.c::monitor_process line 267 — note
// that freq_osr controls WHICH SLICE you're in, not the bin
// spacing within a slice). For JS8 Normal: bin width within a slice
// = symbol rate = 6.25 Hz. Standard JS8 SNR reports in 2500 Hz:
//   correction = -10*log10(2500/6.25) = -26.0 dB
// JS8Call uses -28 (matches a ~1.85x window-ENBW correction for
// Blackman-Harris). gfsk8 uses -32 (different analysis pipeline).
// If on-air comparison to a JS8Call reference shows a consistent
// offset, tune this single constant.
//
// Returns SNR in dB clamped to [-30, +30] (the physically meaningful
// JS8 range — true SNR below ~-26 dB cannot produce a JS8 Normal
// decode), or NANOJS8_ACTIVITY_SNR_NA on out-of-range candidate /
// degenerate noise floor.
//
// CRITICAL bin-indexing note (L7.13-fix2): adjacent BINS in one
// waterfall slice are at TONE spacing (6.25 Hz), not FFT-bin
// spacing (3.125 Hz). So tone t is at `mag_cand[sym*block_stride + t]`,
// NOT `mag_cand[sym*block_stride + t*freq_osr]`. This mirrors
// ft8_lib's own ft8_sync_score which uses p8[sm] with sm being the
// tone index 0..7 directly (see decode.c lines 93, 97).
static int8_t compute_snr_db(const ftx_waterfall_t *wf,
                             const ftx_candidate_t *cand)
{
    if (!wf || !cand || !wf->mag) {
        return NANOJS8_ACTIVITY_SNR_NA;
    }

    // Mirror ft8_lib's internal get_cand_mag indexing exactly. This
    // points at symbol 0 of the candidate at the chosen sub-bin.
    int offset = cand->time_offset;
    offset = (offset * wf->time_osr) + cand->time_sub;
    offset = (offset * wf->freq_osr) + cand->freq_sub;
    offset = (offset * wf->num_bins) + cand->freq_offset;
    const WF_ELEM_T *mag_cand = wf->mag + offset;

    const int block_stride = wf->block_stride;

    // 8 tones per symbol (JS8/FT8 8-FSK). ft8_lib does not expose this
    // as a public macro so it stays local.
    const int NUM_TONES_PER_SYM = 8;

    double signal_lin_sum = 0.0;
    double noise_lin_sum  = 0.0;
    int    signal_count   = 0;
    int    noise_count    = 0;

    for (int m = 0; m < FT8_NUM_SYNC; ++m) {
        for (int k = 0; k < FT8_LENGTH_SYNC; ++k) {
            const int sym = FT8_SYNC_OFFSET * m + k;  // 0..6, 36..42, 72..78

            // Out-of-buffer protection — candidate's time_offset plus
            // symbol index must land in the waterfall's stored blocks.
            const int abs_block = cand->time_offset + sym;
            if (abs_block < 0 || abs_block >= wf->num_blocks) {
                continue;
            }

            const WF_ELEM_T *sym_mag = mag_cand + sym * block_stride;
            const uint8_t expected_tone = kFT8_Costas_pattern[k];

            for (int t = 0; t < NUM_TONES_PER_SYM; ++t) {
                // Tones are stored at consecutive bins within a slice
                // (one entry per 6.25 Hz tone — see monitor.c L267).
                // NOT t*freq_osr — that was the L7.13-fix1 bug.
                const int abs_bin = cand->freq_offset + t;
                if (abs_bin < 0 || abs_bin >= wf->num_bins) {
                    continue;
                }

                const float mag_db  = WF_ELEM_MAG(sym_mag[t]);
                const float mag_lin = powf(10.0f, mag_db * 0.1f);

                if (t == expected_tone) {
                    signal_lin_sum += mag_lin;
                    signal_count++;
                } else {
                    noise_lin_sum += mag_lin;
                    noise_count++;
                }
            }
        }
    }

    if (signal_count == 0 || noise_count == 0) {
        return NANOJS8_ACTIVITY_SNR_NA;
    }

    const double signal_avg = signal_lin_sum / (double)signal_count;
    const double noise_avg  = noise_lin_sum  / (double)noise_count;

    // Degenerate noise floor (e.g. all-zero waterfall) — bail rather
    // than divide by ~zero.
    if (noise_avg < 1.0e-12) {
        return NANOJS8_ACTIVITY_SNR_NA;
    }

    // gfsk8 formula: subtract noise contribution from signal estimate
    // because the peak bin contains signal+noise; lower-bound at a
    // tiny floor to keep log10 finite.
    double ratio = (signal_avg - noise_avg) / noise_avg;
    if (ratio < 1.259e-10) ratio = 1.259e-10;

    float snr_db = 10.0f * log10f((float)ratio) - 26.0f;

    // Clamp to physically meaningful JS8 range. -30 is a safe floor:
    // JS8 Normal threshold is ~-26 dB, so anything that decoded
    // should land above -30 with a correct formula. If we see values
    // pinning at -30 on real decodes, the normalization constant
    // (-26) needs adjustment, NOT the floor.
    if (snr_db < -30.0f) snr_db = -30.0f;
    if (snr_db >  30.0f) snr_db =  30.0f;

    return (int8_t)lroundf(snr_db);
}

// GC pass — drop slots that have aged past the timeout. Called from
// both record and take paths so we don't need a periodic task.
static void msg_pending_gc(int64_t now_us)
{
    for (int i = 0; i < MSG_PENDING_SLOTS; ++i) {
        if (s_msg_pending[i].active &&
            (now_us - s_msg_pending[i].header_seen_us) > MSG_PENDING_TIMEOUT_US) {
            ESP_LOGW(TAG,
                "L7.11g.4 MSG pending GC: slot %d expired "
                "(from=%s freq=%u, %lld s old)",
                i, s_msg_pending[i].from_call,
                (unsigned)s_msg_pending[i].freq_hz,
                (long long)((now_us - s_msg_pending[i].header_seen_us) / 1000000));
            s_msg_pending[i].active = false;
        }
    }
}

// Copy a NUL-terminated string into a fixed buffer.
static inline void copy_call(char *dst, size_t dst_n, const char *src)
{
    if (dst_n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dst_n && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// Record a MSG header for later commit. Caller has verified
// from != self and to is_for_me. Replaces any existing slot at the
// same freq (sender retry); else uses a free slot; else evicts the
// oldest active slot.
static void msg_pending_record(const char *from, const char *to,
                                 uint32_t freq_hz, int64_t now_us)
{
    msg_pending_gc(now_us);

    // 1) Match existing slot at same freq → refresh (sender retry).
    for (int i = 0; i < MSG_PENDING_SLOTS; ++i) {
        if (s_msg_pending[i].active &&
            freq_within_tol(s_msg_pending[i].freq_hz, freq_hz,
                             MSG_PENDING_FREQ_TOL_HZ)) {
            ESP_LOGI(TAG,
                "L7.11g.4 MSG pending: slot %d refreshed "
                "(was from=%s, now from=%s, freq=%u)",
                i, s_msg_pending[i].from_call, from, (unsigned)freq_hz);
            copy_call(s_msg_pending[i].from_call,
                       sizeof(s_msg_pending[i].from_call), from);
            copy_call(s_msg_pending[i].to_call,
                       sizeof(s_msg_pending[i].to_call), to);
            s_msg_pending[i].freq_hz        = freq_hz;
            s_msg_pending[i].header_seen_us = now_us;
            return;
        }
    }

    // 2) Find a free slot.
    for (int i = 0; i < MSG_PENDING_SLOTS; ++i) {
        if (!s_msg_pending[i].active) {
            s_msg_pending[i].active = true;
            copy_call(s_msg_pending[i].from_call,
                       sizeof(s_msg_pending[i].from_call), from);
            copy_call(s_msg_pending[i].to_call,
                       sizeof(s_msg_pending[i].to_call), to);
            s_msg_pending[i].freq_hz        = freq_hz;
            s_msg_pending[i].header_seen_us = now_us;
            ESP_LOGI(TAG,
                "L7.11g.4 MSG pending: slot %d armed "
                "from=%s to=%s freq=%u — awaiting body",
                i, from, to, (unsigned)freq_hz);
            return;
        }
    }

    // 3) All full — evict the oldest active slot.
    int oldest_idx     = 0;
    int64_t oldest_age = s_msg_pending[0].header_seen_us;
    for (int i = 1; i < MSG_PENDING_SLOTS; ++i) {
        if (s_msg_pending[i].header_seen_us < oldest_age) {
            oldest_idx = i;
            oldest_age = s_msg_pending[i].header_seen_us;
        }
    }
    ESP_LOGW(TAG,
        "L7.11g.4 MSG pending FULL — evicting slot %d "
        "(was from=%s freq=%u, %lld ms old) for new from=%s freq=%u",
        oldest_idx,
        s_msg_pending[oldest_idx].from_call,
        (unsigned)s_msg_pending[oldest_idx].freq_hz,
        (long long)((now_us - oldest_age) / 1000),
        from, (unsigned)freq_hz);
    copy_call(s_msg_pending[oldest_idx].from_call,
               sizeof(s_msg_pending[oldest_idx].from_call), from);
    copy_call(s_msg_pending[oldest_idx].to_call,
               sizeof(s_msg_pending[oldest_idx].to_call), to);
    s_msg_pending[oldest_idx].freq_hz        = freq_hz;
    s_msg_pending[oldest_idx].header_seen_us = now_us;
}

// On assembler COMPLETE/PARTIAL_TAIL: look for a pending MSG header
// at the matching freq. Returns true if matched (caller commits),
// and clears the slot.
static bool msg_pending_take(uint32_t freq_hz, int64_t now_us,
                              msg_pending_t *out)
{
    msg_pending_gc(now_us);
    for (int i = 0; i < MSG_PENDING_SLOTS; ++i) {
        if (s_msg_pending[i].active &&
            freq_within_tol(s_msg_pending[i].freq_hz, freq_hz,
                             MSG_PENDING_FREQ_TOL_HZ)) {
            *out = s_msg_pending[i];
            s_msg_pending[i].active = false;
            return true;
        }
    }
    return false;
}

// ── L7.11g.6: per-freq QUERY tracker ─────────────────────────────────────────
//
// Bare "QUERY" verb (cmd id 11) is multi-frame: frame 1 carries the
// directed header with verb="QUERY" and empty body; subsequent
// data frames carry the body (e.g. "MSG 73" for QUERY MSG <id>).
//
// Same deferred-commit pattern as msg_pending: header records
// pending; PARTIAL_TAIL/COMPLETE emission takes the pending and
// dispatches based on body content.
//
// QUERY MSGS (cmd id 12) and QUERY CALL (cmd id 13) are SINGLE-FRAME
// verbs (verb string already includes the suffix) and do NOT use
// this tracker.

#define QUERY_PENDING_SLOTS         4
#define QUERY_PENDING_FREQ_TOL_HZ   25
#define QUERY_PENDING_TIMEOUT_US    (60LL * 1000LL * 1000LL)

typedef struct {
    bool     active;
    uint32_t freq_hz;
    char     asker_call[NANOJS8_MAILBOX_CALL_LEN];
    char     to_call   [NANOJS8_MAILBOX_CALL_LEN];   // us or @ALLCALL
    int64_t  header_seen_us;
} query_pending_t;

static query_pending_t s_query_pending[QUERY_PENDING_SLOTS];

static void query_pending_gc(int64_t now_us)
{
    for (int i = 0; i < QUERY_PENDING_SLOTS; ++i) {
        if (s_query_pending[i].active &&
            (now_us - s_query_pending[i].header_seen_us) > QUERY_PENDING_TIMEOUT_US) {
            ESP_LOGW(TAG,
                "L7.11g.6 QUERY pending GC: slot %d expired "
                "(asker=%s freq=%u, %lld s old)",
                i, s_query_pending[i].asker_call,
                (unsigned)s_query_pending[i].freq_hz,
                (long long)((now_us - s_query_pending[i].header_seen_us) / 1000000));
            s_query_pending[i].active = false;
        }
    }
}

static void query_pending_record(const char *asker_call, const char *to_call,
                                   uint32_t freq_hz, int64_t now_us)
{
    query_pending_gc(now_us);

    for (int i = 0; i < QUERY_PENDING_SLOTS; ++i) {
        if (s_query_pending[i].active &&
            freq_within_tol(s_query_pending[i].freq_hz, freq_hz,
                             QUERY_PENDING_FREQ_TOL_HZ)) {
            ESP_LOGI(TAG,
                "L7.11g.6 QUERY pending: slot %d refreshed "
                "(asker=%s freq=%u)",
                i, asker_call, (unsigned)freq_hz);
            copy_call(s_query_pending[i].asker_call,
                       sizeof(s_query_pending[i].asker_call), asker_call);
            copy_call(s_query_pending[i].to_call,
                       sizeof(s_query_pending[i].to_call), to_call);
            s_query_pending[i].freq_hz        = freq_hz;
            s_query_pending[i].header_seen_us = now_us;
            return;
        }
    }
    for (int i = 0; i < QUERY_PENDING_SLOTS; ++i) {
        if (!s_query_pending[i].active) {
            s_query_pending[i].active = true;
            copy_call(s_query_pending[i].asker_call,
                       sizeof(s_query_pending[i].asker_call), asker_call);
            copy_call(s_query_pending[i].to_call,
                       sizeof(s_query_pending[i].to_call), to_call);
            s_query_pending[i].freq_hz        = freq_hz;
            s_query_pending[i].header_seen_us = now_us;
            ESP_LOGI(TAG,
                "L7.11g.6 QUERY pending: slot %d armed "
                "asker=%s to=%s freq=%u — awaiting body",
                i, asker_call, to_call, (unsigned)freq_hz);
            return;
        }
    }
    // All full — evict oldest.
    int oldest_idx     = 0;
    int64_t oldest_age = s_query_pending[0].header_seen_us;
    for (int i = 1; i < QUERY_PENDING_SLOTS; ++i) {
        if (s_query_pending[i].header_seen_us < oldest_age) {
            oldest_idx = i;
            oldest_age = s_query_pending[i].header_seen_us;
        }
    }
    ESP_LOGW(TAG,
        "L7.11g.6 QUERY pending FULL — evicting slot %d (asker=%s)",
        oldest_idx, s_query_pending[oldest_idx].asker_call);
    copy_call(s_query_pending[oldest_idx].asker_call,
               sizeof(s_query_pending[oldest_idx].asker_call), asker_call);
    copy_call(s_query_pending[oldest_idx].to_call,
               sizeof(s_query_pending[oldest_idx].to_call), to_call);
    s_query_pending[oldest_idx].freq_hz        = freq_hz;
    s_query_pending[oldest_idx].header_seen_us = now_us;
}

static bool query_pending_take(uint32_t freq_hz, int64_t now_us,
                                query_pending_t *out)
{
    query_pending_gc(now_us);
    for (int i = 0; i < QUERY_PENDING_SLOTS; ++i) {
        if (s_query_pending[i].active &&
            freq_within_tol(s_query_pending[i].freq_hz, freq_hz,
                             QUERY_PENDING_FREQ_TOL_HZ)) {
            *out = s_query_pending[i];
            s_query_pending[i].active = false;
            return true;
        }
    }
    return false;
}

// ── L7.11g.7-fix1: per-freq compound-from chaining ───────────────────────────
//
// JS8 splits messages from compound callsigns or addressed to compound/
// group targets (e.g. "@GHOSTNET") into two on-air frames:
//
//   Frame 1 (FRAME_COMPOUND, type=1):  "<FROM>: "       (announces sender)
//   Frame 2 (FRAME_COMPOUND_DIRECTED, type=2):
//                                       "<COMPOUND_TO> <VERB...>"
//
// Both frames are at the same audio freq, one slot apart (15s for
// JS8 Normal). The codec sees them as independent decodes; this
// per-freq tracker remembers the from_call from Frame 1 so we can
// populate it into the type-2 decode when the verb-detection
// handlers run.
//
// Without this chaining @GHOSTNET QUERY MSGS arrives with msg.from_call
// empty (the codec only saw Frame 2) and the QUERY MSGS handler can't
// route a reply.
//
// 4 slots × ~80 B ≈ 320 B BSS. Timeout 30 s = 2 JS8 Normal slots.
// Peek-only on take (do NOT consume) — one compound-FROM may pair
// with multiple compound-DIRECTED frames in the same slot if multiple
// verbs are sent in sequence.

#define COMPOUND_FROM_SLOTS         4
#define COMPOUND_FROM_FREQ_TOL_HZ   25
#define COMPOUND_FROM_TIMEOUT_US    (30LL * 1000LL * 1000LL)

typedef struct {
    bool     active;
    uint32_t freq_hz;
    char     from_call[NANOJS8_MAILBOX_CALL_LEN];
    int64_t  recorded_us;
} compound_from_pending_t;

static compound_from_pending_t s_compound_from[COMPOUND_FROM_SLOTS];

static void compound_from_gc(int64_t now_us)
{
    for (int i = 0; i < COMPOUND_FROM_SLOTS; ++i) {
        if (s_compound_from[i].active &&
            (now_us - s_compound_from[i].recorded_us) > COMPOUND_FROM_TIMEOUT_US) {
            s_compound_from[i].active = false;
        }
    }
}

static void compound_from_record(const char *from_call, uint32_t freq_hz,
                                   int64_t now_us)
{
    compound_from_gc(now_us);

    // Replace any existing entry at this freq.
    for (int i = 0; i < COMPOUND_FROM_SLOTS; ++i) {
        if (s_compound_from[i].active &&
            freq_within_tol(s_compound_from[i].freq_hz, freq_hz,
                             COMPOUND_FROM_FREQ_TOL_HZ)) {
            copy_call(s_compound_from[i].from_call,
                       sizeof(s_compound_from[i].from_call), from_call);
            s_compound_from[i].freq_hz     = freq_hz;
            s_compound_from[i].recorded_us = now_us;
            ESP_LOGI(TAG,
                "L7.11g.7-fix1 compound-FROM: refreshed slot %d "
                "from=%s freq=%u",
                i, from_call, (unsigned)freq_hz);
            return;
        }
    }
    // Free slot.
    for (int i = 0; i < COMPOUND_FROM_SLOTS; ++i) {
        if (!s_compound_from[i].active) {
            s_compound_from[i].active = true;
            copy_call(s_compound_from[i].from_call,
                       sizeof(s_compound_from[i].from_call), from_call);
            s_compound_from[i].freq_hz     = freq_hz;
            s_compound_from[i].recorded_us = now_us;
            ESP_LOGI(TAG,
                "L7.11g.7-fix1 compound-FROM: armed slot %d "
                "from=%s freq=%u — awaiting compound-DIRECTED",
                i, from_call, (unsigned)freq_hz);
            return;
        }
    }
    // Full — evict oldest.
    int oldest_idx     = 0;
    int64_t oldest_age = s_compound_from[0].recorded_us;
    for (int i = 1; i < COMPOUND_FROM_SLOTS; ++i) {
        if (s_compound_from[i].recorded_us < oldest_age) {
            oldest_idx = i;
            oldest_age = s_compound_from[i].recorded_us;
        }
    }
    copy_call(s_compound_from[oldest_idx].from_call,
               sizeof(s_compound_from[oldest_idx].from_call), from_call);
    s_compound_from[oldest_idx].freq_hz     = freq_hz;
    s_compound_from[oldest_idx].recorded_us = now_us;
    ESP_LOGW(TAG,
        "L7.11g.7-fix1 compound-FROM FULL — evicted slot %d for from=%s",
        oldest_idx, from_call);
}

// Peek (do NOT consume) — same freq, returns true and writes call.
static bool compound_from_peek(uint32_t freq_hz, int64_t now_us,
                                 char *out_call, size_t out_call_n)
{
    compound_from_gc(now_us);
    for (int i = 0; i < COMPOUND_FROM_SLOTS; ++i) {
        if (s_compound_from[i].active &&
            freq_within_tol(s_compound_from[i].freq_hz, freq_hz,
                             COMPOUND_FROM_FREQ_TOL_HZ)) {
            copy_call(out_call, out_call_n, s_compound_from[i].from_call);
            return true;
        }
    }
    return false;
}

// ── L7.11g.7: per-freq MSG TO: tracker ───────────────────────────────────────
//
// "MSG TO:" verb (cmd id 10) is the relay command. Wire format:
//   <FROM>: <TO>  MSG TO:<FOR> <TEXT>
//
// Frame 1 carries the directed header with verb="MSG TO:" (after fix1
// trim) and empty body. Subsequent data frames carry the recipient
// callsign and message text: "<FOR> <TEXT>".
//
// Same per-freq deferred-commit pattern as msg_pending/query_pending:
// header records pending; PARTIAL_TAIL/COMPLETE assembler emission
// takes the pending and dispatches by parsing <FOR> <TEXT> from the
// joined body.
//
// On commit: add_store(from=sender, to=FOR, body=TEXT) + ACK to sender.
// The relayed message then sits as STORE for FOR and will be returned
// via the existing L7.11g.6 QUERY MSGS handler when FOR queries us.

#define MSG_TO_PENDING_SLOTS         4
#define MSG_TO_PENDING_FREQ_TOL_HZ   25
#define MSG_TO_PENDING_TIMEOUT_US    (60LL * 1000LL * 1000LL)

typedef struct {
    bool     active;
    uint32_t freq_hz;
    char     sender_call[NANOJS8_MAILBOX_CALL_LEN];   // originator
    char     to_call    [NANOJS8_MAILBOX_CALL_LEN];   // us (the relay node)
    int64_t  header_seen_us;
} msg_to_pending_t;

static msg_to_pending_t s_msg_to_pending[MSG_TO_PENDING_SLOTS];

static void msg_to_pending_gc(int64_t now_us)
{
    for (int i = 0; i < MSG_TO_PENDING_SLOTS; ++i) {
        if (s_msg_to_pending[i].active &&
            (now_us - s_msg_to_pending[i].header_seen_us) > MSG_TO_PENDING_TIMEOUT_US) {
            ESP_LOGW(TAG,
                "L7.11g.7 MSG-TO pending GC: slot %d expired "
                "(sender=%s freq=%u, %lld s old)",
                i, s_msg_to_pending[i].sender_call,
                (unsigned)s_msg_to_pending[i].freq_hz,
                (long long)((now_us - s_msg_to_pending[i].header_seen_us) / 1000000));
            s_msg_to_pending[i].active = false;
        }
    }
}

static void msg_to_pending_record(const char *sender_call, const char *to_call,
                                    uint32_t freq_hz, int64_t now_us)
{
    msg_to_pending_gc(now_us);

    for (int i = 0; i < MSG_TO_PENDING_SLOTS; ++i) {
        if (s_msg_to_pending[i].active &&
            freq_within_tol(s_msg_to_pending[i].freq_hz, freq_hz,
                             MSG_TO_PENDING_FREQ_TOL_HZ)) {
            ESP_LOGI(TAG,
                "L7.11g.7 MSG-TO pending: slot %d refreshed "
                "(sender=%s freq=%u)",
                i, sender_call, (unsigned)freq_hz);
            copy_call(s_msg_to_pending[i].sender_call,
                       sizeof(s_msg_to_pending[i].sender_call), sender_call);
            copy_call(s_msg_to_pending[i].to_call,
                       sizeof(s_msg_to_pending[i].to_call), to_call);
            s_msg_to_pending[i].freq_hz        = freq_hz;
            s_msg_to_pending[i].header_seen_us = now_us;
            return;
        }
    }
    for (int i = 0; i < MSG_TO_PENDING_SLOTS; ++i) {
        if (!s_msg_to_pending[i].active) {
            s_msg_to_pending[i].active = true;
            copy_call(s_msg_to_pending[i].sender_call,
                       sizeof(s_msg_to_pending[i].sender_call), sender_call);
            copy_call(s_msg_to_pending[i].to_call,
                       sizeof(s_msg_to_pending[i].to_call), to_call);
            s_msg_to_pending[i].freq_hz        = freq_hz;
            s_msg_to_pending[i].header_seen_us = now_us;
            ESP_LOGI(TAG,
                "L7.11g.7 MSG-TO pending: slot %d armed "
                "sender=%s to=%s freq=%u — awaiting body",
                i, sender_call, to_call, (unsigned)freq_hz);
            return;
        }
    }
    // All full — evict oldest.
    int oldest_idx     = 0;
    int64_t oldest_age = s_msg_to_pending[0].header_seen_us;
    for (int i = 1; i < MSG_TO_PENDING_SLOTS; ++i) {
        if (s_msg_to_pending[i].header_seen_us < oldest_age) {
            oldest_idx = i;
            oldest_age = s_msg_to_pending[i].header_seen_us;
        }
    }
    ESP_LOGW(TAG,
        "L7.11g.7 MSG-TO pending FULL — evicting slot %d (sender=%s)",
        oldest_idx, s_msg_to_pending[oldest_idx].sender_call);
    copy_call(s_msg_to_pending[oldest_idx].sender_call,
               sizeof(s_msg_to_pending[oldest_idx].sender_call), sender_call);
    copy_call(s_msg_to_pending[oldest_idx].to_call,
               sizeof(s_msg_to_pending[oldest_idx].to_call), to_call);
    s_msg_to_pending[oldest_idx].freq_hz        = freq_hz;
    s_msg_to_pending[oldest_idx].header_seen_us = now_us;
}

static bool msg_to_pending_take(uint32_t freq_hz, int64_t now_us,
                                  msg_to_pending_t *out)
{
    msg_to_pending_gc(now_us);
    for (int i = 0; i < MSG_TO_PENDING_SLOTS; ++i) {
        if (s_msg_to_pending[i].active &&
            freq_within_tol(s_msg_to_pending[i].freq_hz, freq_hz,
                             MSG_TO_PENDING_FREQ_TOL_HZ)) {
            *out = s_msg_to_pending[i];
            s_msg_to_pending[i].active = false;
            return true;
        }
    }
    return false;
}

// Parse "<FOR> <TEXT>" body from a MSG TO continuation.
// On success: writes recipient call into for_buf (uppercase) and
// returns a pointer into `body` to the start of TEXT (after the
// space separating FOR from TEXT). On failure (empty body, missing
// space, oversize callsign): returns NULL.
//
// FOR is uppercased to match callsign conventions. TEXT is returned
// as-is (no upper-casing — the operator's intended content).
static const char *parse_msg_to_body(const char *body, char *for_buf,
                                      size_t for_buf_n)
{
    if (!body || !for_buf || for_buf_n == 0) return NULL;

    const char *p = body;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') return NULL;     // empty body

    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (i >= for_buf_n - 1) return NULL;  // overflow
        // Uppercase
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        for_buf[i++] = c;
        ++p;
    }
    if (i == 0) return NULL;
    for_buf[i] = '\0';

    // Must have a separator before TEXT, otherwise no TEXT.
    if (*p != ' ' && *p != '\t') return NULL;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') return NULL;     // empty TEXT after FOR

    return p;
}

// ── L7.11g.6: outbound-delivery tracker ──────────────────────────────────────
//
// Per JS8Call/MicroJS8 protocol, a STORE entry transitions to
// DELIVERED only after the recipient ACKs the body delivery. We
// track each "X MSG <id> <body>" send and wait for X's verb=ACK.
// On match: mark_delivered(id), clear slot.
//
// Timeout 120 s — gives the recipient up to ~4 slot pairs to ACK
// (decode + queue + slot-align + TX). If ACK doesn't arrive the
// STORE row stays STORE, will be re-delivered on next QUERY MSGS.
//
// 4 slots ≈ 100 B BSS. Same eviction rules as the other trackers.

#define OUT_DELIVERY_SLOTS         4
#define OUT_DELIVERY_TIMEOUT_US    (120LL * 1000LL * 1000LL)

typedef struct {
    bool     active;
    char     asker_call[NANOJS8_MAILBOX_CALL_LEN];
    uint16_t inbox_id;
    int64_t  delivered_at_us;
} out_delivery_t;

static out_delivery_t s_out_delivery[OUT_DELIVERY_SLOTS];

static void out_delivery_gc(int64_t now_us)
{
    for (int i = 0; i < OUT_DELIVERY_SLOTS; ++i) {
        if (s_out_delivery[i].active &&
            (now_us - s_out_delivery[i].delivered_at_us) > OUT_DELIVERY_TIMEOUT_US) {
            ESP_LOGW(TAG,
                "L7.11g.6 OUT delivery GC: slot %d expired "
                "(asker=%s id=%u, no ACK in %lld s — STORE stays STORE)",
                i, s_out_delivery[i].asker_call,
                (unsigned)s_out_delivery[i].inbox_id,
                (long long)((now_us - s_out_delivery[i].delivered_at_us) / 1000000));
            s_out_delivery[i].active = false;
        }
    }
}

static void out_delivery_record(const char *asker_call, uint16_t inbox_id,
                                  int64_t now_us)
{
    out_delivery_gc(now_us);

    for (int i = 0; i < OUT_DELIVERY_SLOTS; ++i) {
        if (!s_out_delivery[i].active) {
            s_out_delivery[i].active = true;
            copy_call(s_out_delivery[i].asker_call,
                       sizeof(s_out_delivery[i].asker_call), asker_call);
            s_out_delivery[i].inbox_id        = inbox_id;
            s_out_delivery[i].delivered_at_us = now_us;
            ESP_LOGI(TAG,
                "L7.11g.6 OUT delivery: slot %d armed asker=%s id=%u",
                i, asker_call, (unsigned)inbox_id);
            return;
        }
    }
    int oldest_idx     = 0;
    int64_t oldest_age = s_out_delivery[0].delivered_at_us;
    for (int i = 1; i < OUT_DELIVERY_SLOTS; ++i) {
        if (s_out_delivery[i].delivered_at_us < oldest_age) {
            oldest_idx = i;
            oldest_age = s_out_delivery[i].delivered_at_us;
        }
    }
    ESP_LOGW(TAG,
        "L7.11g.6 OUT delivery FULL — evicting slot %d "
        "(asker=%s id=%u) for asker=%s id=%u",
        oldest_idx, s_out_delivery[oldest_idx].asker_call,
        (unsigned)s_out_delivery[oldest_idx].inbox_id,
        asker_call, (unsigned)inbox_id);
    copy_call(s_out_delivery[oldest_idx].asker_call,
               sizeof(s_out_delivery[oldest_idx].asker_call), asker_call);
    s_out_delivery[oldest_idx].inbox_id        = inbox_id;
    s_out_delivery[oldest_idx].delivered_at_us = now_us;
}

// Match an incoming ACK against pending deliveries. Returns true if
// matched and clears the slot. Caller uses the id to mark_delivered.
static bool out_delivery_match_ack(const char *acker_call, int64_t now_us,
                                     uint16_t *out_inbox_id)
{
    out_delivery_gc(now_us);
    for (int i = 0; i < OUT_DELIVERY_SLOTS; ++i) {
        if (s_out_delivery[i].active &&
            strcasecmp(s_out_delivery[i].asker_call, acker_call) == 0) {
            *out_inbox_id = s_out_delivery[i].inbox_id;
            s_out_delivery[i].active = false;
            return true;
        }
    }
    return false;
}

// Parse "MSG <id>" from a body string. Tolerates whitespace and
// trailing CRC-pad garbage (e.g. "MSG 73 Y?L"). Returns the id, or
// 0 if not parseable. id=0 isn't a valid mailbox id (auto-increment
// starts at 1) so 0 is unambiguous as a sentinel.
static uint16_t parse_query_msg_id(const char *body)
{
    if (!body) return 0;
    while (*body == ' ' || *body == '\t') ++body;
    // Match "MSG " (case-sensitive — protocol is uppercase)
    if (body[0] != 'M' || body[1] != 'S' || body[2] != 'G') return 0;
    if (body[3] != ' ' && body[3] != '\t') return 0;
    body += 4;
    while (*body == ' ' || *body == '\t') ++body;
    // Parse digits
    uint32_t n = 0;
    int digits = 0;
    while (*body >= '0' && *body <= '9') {
        n = n * 10u + (uint32_t)(*body - '0');
        if (n > 65535u) return 0;   // overflow
        ++body;
        ++digits;
    }
    if (digits == 0) return 0;
    return (uint16_t)n;
}

// ── Main task ────────────────────────────────────────────────────────────────

static void sync_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Sync task starting (JS8 Normal sync detection)");

    // 1. Allocate slot snapshot buffer in PSRAM.
    const size_t snap_bytes = NANOJS8_RX_AUDIO_SLOT_SAMPLES * sizeof(int16_t);
    s_snapshot = (int16_t *)heap_caps_malloc(snap_bytes, MALLOC_CAP_SPIRAM);
    if (!s_snapshot) {
        ESP_LOGE(TAG, "Snapshot buffer alloc failed (%zu B PSRAM)", snap_bytes);
        vTaskDeleteWithCaps(NULL);  // L7.14-fix7: matches WithCaps create
        return;
    }
    ESP_LOGI(TAG, "Snapshot buffer: %zu KB PSRAM", snap_bytes / 1024);

    // 2. Initialize the monitor. Configure as FT8 protocol; the Costas
    //    pattern has been replaced in ft8_lib's constants.c so the sync
    //    scorer effectively searches for JS8 Normal frames.
    const monitor_config_t mon_cfg = {
        .f_min       = JS8_SYNC_F_MIN,
        .f_max       = JS8_SYNC_F_MAX,
        .sample_rate = JS8_SYNC_SAMPLE_RATE,
        .time_osr    = JS8_SYNC_TIME_OSR,
        .freq_osr    = JS8_SYNC_FREQ_OSR,
        .protocol    = FTX_PROTOCOL_FT8,
    };
    monitor_init(&s_mon, &mon_cfg);

    // Verify the monitor came up OK. If the waterfall heap-alloc failed
    // (PSRAM exhausted) monitor.c logs and returns with me->wf.mag = NULL.
    if (s_mon.wf.mag == NULL) {
        ESP_LOGE(TAG, "monitor_init failed — PSRAM exhausted? "
                      "(needed waterfall %d × %d × %d × %d bytes)",
                 s_mon.wf.max_blocks, s_mon.wf.time_osr, s_mon.wf.freq_osr,
                 s_mon.wf.num_bins);
        heap_caps_free(s_snapshot);
        vTaskDeleteWithCaps(NULL);  // L7.14-fix7: matches WithCaps create
        return;
    }

    ESP_LOGI(TAG, "Monitor up: nfft=%d block=%d subblock=%d "
                  "bins=%d max_blocks=%d t_osr=%d f_osr=%d",
             s_mon.nfft, s_mon.block_size, s_mon.subblock_size,
             s_mon.wf.num_bins, s_mon.wf.max_blocks,
             s_mon.wf.time_osr, s_mon.wf.freq_osr);
    ESP_LOGI(TAG, "Post-init heap: internal=%u B  psram=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Sanity: block_size must equal BLOCK_SIZE so our chunk buffer is the
    // right size for monitor_process.
    if ((uint32_t)s_mon.block_size != BLOCK_SIZE) {
        ESP_LOGE(TAG, "BLOCK_SIZE mismatch: ft8_lib expects %d, we have %u",
                 s_mon.block_size, (unsigned)BLOCK_SIZE);
        // Continue anyway; results undefined.
    }

    // 3. Wait for UTC. rx_audio's slot trigger is also gated on this.
    if (!nanojs8_time_is_set()) {
        ESP_LOGI(TAG, "Waiting for UTC (operator must enter via SETUP)");
        while (!nanojs8_time_is_set()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "UTC set — sync detector armed");

    // Prime the watermark so we don't process slots fired before we
    // were ready.
    {
        nanojs8_rx_audio_stats_t s;
        nanojs8_rx_audio_get_stats(&s);
        s_last_seen_slots = s.slots_fired;
    }

    // 4. Main poll loop.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));

        nanojs8_rx_audio_stats_t rxs;
        nanojs8_rx_audio_get_stats(&rxs);

        if (rxs.slots_fired == s_last_seen_slots) {
            continue;
        }
        // If multiple slots fired (e.g. very heavy CPU load missed one),
        // we can only recover the LATEST from the ring. Update watermark
        // unconditionally and process the latest.
        s_last_seen_slots = rxs.slots_fired;

        if (!nanojs8_rx_audio_snapshot_last_slot(
                s_snapshot, NANOJS8_RX_AUDIO_SLOT_SAMPLES)) {
            ESP_LOGW(TAG, "snapshot_last_slot returned false; skipping slot");
            continue;
        }

        // Reset monitor state for this slot.
        monitor_reset(&s_mon);

        const uint64_t t0 = esp_timer_get_time();

        // Feed the slot to the monitor in block_size chunks.
        const uint32_t num_blocks =
            NANOJS8_RX_AUDIO_SLOT_SAMPLES / BLOCK_SIZE;
        for (uint32_t blk = 0; blk < num_blocks; ++blk) {
            const int16_t *src = s_snapshot + (blk * BLOCK_SIZE);
            // int16 [-32768, 32767] → float [-1.0, 1.0)
            // Compiler will auto-vectorize this on ESP32-S3.
            for (uint32_t i = 0; i < BLOCK_SIZE; ++i) {
                s_chunk[i] = (float)src[i] * (1.0f / 32768.0f);
            }
            monitor_process(&s_mon, s_chunk);
        }

        // Find candidates. ftx_find_candidates does a min-heap of the
        // top N by Costas correlation score; with min_score threshold,
        // candidates scoring below MIN_SCORE are not entered.
        const int n_cand = ftx_find_candidates(
            &s_mon.wf, JS8_SYNC_MAX_CAND, s_cand_heap, JS8_SYNC_MIN_SCORE);

        // L7.6: For each candidate, try to extract LLRs and run the
        // LDPC + CRC pipeline. The BP decoder has built-in early-stopping
        // for unconvergent paths (typically <10 ms for noise candidates),
        // so attempting every candidate is cheap. We track:
        //   attempts = candidates we tried
        //   decodes  = candidates whose CRC validated
        //
        // L7.8: Within a slot, multiple candidates often decode to the same
        // raw 12-char string (the sync finder reports adjacent time/freq
        // offsets of the same signal). We dedupe by raw text before logging
        // and before feeding the assembler — otherwise the same message
        // would be appended N times to the multi-frame buffer.
        float llr_buf[NANOJS8_JS8_LDPC_N];
        uint32_t slot_attempts = 0;
        uint32_t slot_decodes  = 0;

        // Dedup cache for this slot. JS8 Normal payload is 12 chars; we
        // hold up to JS8_SYNC_MAX_CAND seen-raws (one per candidate is
        // the worst case). The cache lives on the stack — ~360 bytes.
        char     seen_raws[JS8_SYNC_MAX_CAND][NANOJS8_JS8_MSG_CHARS + 1];
        int      seen_count = 0;

        for (int i = 0; i < n_cand; ++i) {
            ++slot_attempts;

            // L7.11f-fix2c: yield so IDLE1 can run periodically while
            // we grind through candidates. Busy slots see 30 candidates
            // with cpu>5 s total, which would otherwise starve IDLE1
            // long enough to trip the default 5 s task-watchdog. One
            // tick (10 ms) per candidate × 30 = 300 ms added wall time
            // on the worst slot (~4.6% slowdown). Quiet slots (3
            // candidates) pay only 30 ms.
            vTaskDelay(pdMS_TO_TICKS(1));

            const ftx_candidate_t *c = &s_cand_heap[i];

            // Extract 174 normalised LLRs from the candidate's waterfall
            // position. JS8 natural-binary tone-to-bit mapping (no Gray).
            nanojs8_js8_extract_llrs(&s_mon.wf, c, llr_buf);

            // Run LDPC + CRC. Returns true only on full success (BP
            // converged AND CRC verified).
            nanojs8_js8_decode_result_t dec = {0};
            if (!nanojs8_js8_decode_llrs(llr_buf, &dec)) {
                continue;
            }

            // L7.8: dedupe by raw 12 chars. If we already processed this
            // exact codeword in this slot, skip it.
            bool duplicate = false;
            for (int k = 0; k < seen_count; ++k) {
                if (memcmp(seen_raws[k], dec.message,
                           NANOJS8_JS8_MSG_CHARS) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (seen_count < JS8_SYNC_MAX_CAND) {
                memcpy(seen_raws[seen_count], dec.message,
                       NANOJS8_JS8_MSG_CHARS);
                seen_raws[seen_count][NANOJS8_JS8_MSG_CHARS] = '\0';
                ++seen_count;
            }

            // Successful unique decode — count it.
            ++slot_decodes;

            // L7.13: compute real radio SNR once per successful decode.
            // In scope for record_decode + both mailbox_add_unread call
            // sites further down in this loop iteration. Failure
            // (out-of-range candidate, degenerate noise floor) returns
            // NANOJS8_ACTIVITY_SNR_NA — downstream consumers treat that
            // as "blank" rather than 0 dB.
            const int8_t snr_db = compute_snr_db(&s_mon.wf, c);
            const float bin_hz_now =
                (float)JS8_SYNC_SAMPLE_RATE / (float)s_mon.nfft;
            const float freq_hz_dec =
                ((float)c->freq_offset + (float)c->freq_sub /
                                            (float)JS8_SYNC_FREQ_OSR) * bin_hz_now
                + JS8_SYNC_F_MIN;
            const float dt_s_dec =
                ((float)c->time_offset + (float)c->time_sub /
                                            (float)JS8_SYNC_TIME_OSR) *
                (1.0f / 6.25f);

            // L7.7: Try to unpack the 12 raw chars into human-readable text
            // via the JS8 protocol-layer (Varicode strategies). Falls back
            // to raw display if no strategy matched.
            nanojs8_js8_message_t msg = {0};
            const bool parsed = nanojs8_js8_unpack_message(
                dec.message, dec.frame_type, NANOJS8_JS8_SUBMODE_NORMAL, &msg);

            // Build UTC timestamp string once for both log paths.
            uint8_t uh = 0, um = 0, us_ = 0;
            char tbuf[12] = "??:??:??";
            if (nanojs8_time_get_utc(&uh, &um, &us_)) {
                snprintf(tbuf, sizeof(tbuf), "%02u:%02u:%02u",
                         (unsigned)uh, (unsigned)um, (unsigned)us_);
            }

            if (parsed) {
                // L7.13-fix3: include the real SNR in the log so we can
                // calibrate against a JS8Call reference without needing
                // the HEARD screen visible. NA sentinel renders as 'NA'
                // — distinguishable from a real 0 dB value.
                char snr_str[8];
                if (snr_db == NANOJS8_ACTIVITY_SNR_NA) {
                    snprintf(snr_str, sizeof(snr_str), "NA ");
                } else {
                    snprintf(snr_str, sizeof(snr_str), "%+03d", (int)snr_db);
                }
                ESP_LOGI(TAG, "RX %s  score=%2d  snr=%s  freq=%6.1f Hz  "
                              "dt=%+5.2f s  type=%u sub=%u  msg=\"%s\"  "
                              "raw=\"%.12s\"",
                         tbuf, (int)c->score, snr_str,
                         (double)freq_hz_dec, (double)dt_s_dec,
                         (unsigned)dec.frame_type,
                         (unsigned)msg.frame_subtype,
                         msg.text, dec.message);

                // L7.11g.7-fix1: chain compound-FROM and
                // compound-DIRECTED frames at the same audio freq.
                //
                // FRAME_COMPOUND (type=1) carries the sender's call
                // and arrives one slot before the actual directed
                // message. Record it for the freq.
                //
                // FRAME_COMPOUND_DIRECTED (type=2) carries the
                // destination + verb but no sender. If we have a
                // recent compound-FROM at the same freq, populate
                // msg.from_call from it so the verb-detection
                // handlers below can route correctly.
                const int64_t now_us = esp_timer_get_time();

                // L7.15: substitute the "<....>" nonstd-call
                // placeholder in msg.from_call (and msg.text)
                // when we have a matching compound-FROM at the
                // same audio freq. The Varicode unpack returns
                // this literal placeholder for any compound
                // sender (e.g. "W5DMH/P", "KH6/W5DMH", club
                // calls); the actual call was announced one
                // slot earlier via a FrameCompound frame which
                // we already record in compound_from_pending
                // for the FRAME_COMPOUND_DIRECTED chain below.
                //
                // PSRAM/BSS cost: 0 (reuses the existing 320 B
                // tracker). Only fires when msg.from_call is
                // the exact "<....>" placeholder; falls through
                // unchanged when the peek misses, so handlers
                // downstream see today's behavior on no-record.
                {
                    static const char *NONSTD_PLACEHOLDER = "<....>";
                    if (strcmp(msg.from_call,
                                NONSTD_PLACEHOLDER) == 0) {
                        char chained[NANOJS8_MAILBOX_CALL_LEN];
                        if (compound_from_peek(
                                (uint32_t)freq_hz_dec, now_us,
                                chained, sizeof(chained))) {
                            // Substitute the structured field.
                            copy_call(msg.from_call,
                                       sizeof(msg.from_call),
                                       chained);
                            // Substitute the placeholder inside
                            // msg.text too, so the RX log line
                            // and the assembler's output (which
                            // feeds the HEARD/DIRECTED screens
                            // for single-frame messages) show
                            // the real call. memmove handles
                            // the size delta (placeholder is
                            // 6 chars, chained is up to 15 + NUL).
                            char *p = strstr(msg.text,
                                              NONSTD_PLACEHOLDER);
                            if (p) {
                                const size_t ph_len =
                                    strlen(NONSTD_PLACEHOLDER);
                                const size_t ch_len =
                                    strlen(chained);
                                const size_t tail_len =
                                    strlen(p + ph_len);
                                const size_t prefix_off =
                                    (size_t)(p - msg.text);
                                if (prefix_off + ch_len +
                                    tail_len + 1 <=
                                    sizeof(msg.text)) {
                                    memmove(p + ch_len,
                                             p + ph_len,
                                             tail_len + 1);
                                    memcpy(p, chained, ch_len);
                                }
                            }
                            ESP_LOGI(TAG,
                                "L7.15 nonstd-FROM substituted: "
                                "<....> → '%s' at freq=%u "
                                "(type=%u sub=%u)",
                                chained, (unsigned)freq_hz_dec,
                                (unsigned)dec.frame_type,
                                (unsigned)msg.frame_subtype);
                        } else {
                            ESP_LOGD(TAG,
                                "L7.15 nonstd-FROM placeholder at "
                                "freq=%u has no recent compound-"
                                "FROM record — leaving as <....>",
                                (unsigned)freq_hz_dec);
                        }
                    }
                }

                if (msg.frame_subtype == NANOJS8_JS8_FRAME_COMPOUND &&
                    msg.from_call[0] != '\0') {
                    compound_from_record(msg.from_call,
                                          (uint32_t)freq_hz_dec, now_us);
                } else if (msg.frame_subtype ==
                            NANOJS8_JS8_FRAME_COMPOUND_DIRECTED &&
                            msg.from_call[0] == '\0') {
                    char chained[NANOJS8_MAILBOX_CALL_LEN];
                    if (compound_from_peek((uint32_t)freq_hz_dec,
                                            now_us,
                                            chained, sizeof(chained))) {
                        // Populate msg.from_call so the handlers see it.
                        copy_call(msg.from_call, sizeof(msg.from_call),
                                   chained);
                        ESP_LOGI(TAG,
                            "L7.11g.7-fix1 chained compound-FROM '%s' "
                            "→ compound-DIRECTED at freq=%u "
                            "(to=%s verb='%s')",
                            chained, (unsigned)freq_hz_dec,
                            msg.to_call, msg.verb);
                    } else {
                        ESP_LOGW(TAG,
                            "L7.11g.7-fix1 compound-DIRECTED at "
                            "freq=%u has no recent compound-FROM "
                            "(to=%s verb='%s') — handlers may skip "
                            "this frame",
                            (unsigned)freq_hz_dec,
                            msg.to_call, msg.verb);
                    }
                }
            } else {
                char snr_str2[8];
                if (snr_db == NANOJS8_ACTIVITY_SNR_NA) {
                    snprintf(snr_str2, sizeof(snr_str2), "NA ");
                } else {
                    snprintf(snr_str2, sizeof(snr_str2), "%+03d", (int)snr_db);
                }
                ESP_LOGI(TAG, "RX %s  score=%2d  snr=%s  freq=%6.1f Hz  "
                              "dt=%+5.2f s  type=%u sub=---  "
                              "msg=[unparsed]  raw=\"%.12s\"",
                         tbuf, (int)c->score, snr_str2,
                         (double)freq_hz_dec, (double)dt_s_dec,
                         (unsigned)dec.frame_type, dec.message);
            }

            // L7.8: feed the assembler. Data frames (FRAME_DATA / DATA_COMPRESSED)
            // buffer until LAST arrives; everything else emits as SINGLE_FRAME.
            // Only feed parsed frames — an unparsed frame has no text to add.
            if (parsed) {
                const uint32_t now_ms =
                    (uint32_t)(esp_timer_get_time() / 1000);
                nanojs8_js8_assembly_t asm_out;
                const bool have_msg = nanojs8_js8_assemble_frame(
                    freq_hz_dec, dec.frame_type, msg.frame_subtype,
                    msg.text, now_ms, &asm_out);

                if (have_msg) {
                    const char *kind_str =
                        (asm_out.kind == NANOJS8_JS8_ASM_KIND_COMPLETE)      ? "COMPLETE"     :
                        (asm_out.kind == NANOJS8_JS8_ASM_KIND_SINGLE_FRAME)  ? "SINGLE"       :
                        (asm_out.kind == NANOJS8_JS8_ASM_KIND_PARTIAL_TAIL)  ? "PARTIAL_TAIL" :
                                                                               "?";
                    ESP_LOGI(TAG, "MSG %s  freq=%6.1f Hz  kind=%s  "
                                  "frames=%u  text=\"%s\"",
                             tbuf, (double)asm_out.freq_hz, kind_str,
                             (unsigned)asm_out.frame_count, asm_out.text);
                }

                // L7.9: feed the activity store. This is a single call —
                // the activity module decides which of HEARD / DIRECTED to
                // update based on which structured fields are populated.
                // Single-thread call site (decode loop) → no race with
                // the UI task, which uses snapshots through the mutex.
                nanojs8_activity_record_decode(
                    msg.from_call,    // FROM callsign (may be empty for pure-data)
                    msg.to_call,      // TO callsign  (empty if heartbeat / broadcast)
                    msg.verb,         // verb string  (empty for pure-data chunks)
                    msg.body,         // numeric arg / grid / chunk body
                    msg.grid,         // sender's grid (only set on heartbeats)
                    (int)c->score,
                    snr_db,           // L7.13: real radio SNR from compute_snr_db
                    freq_hz_dec);

                // L7.11g.4-fix1: MSG verb → defer commit until body arrives.
                //
                // Original L7.11g.4 fired mailbox+ACK on the header
                // frame. Two bugs surfaced from on-air testing with
                // KD8PGB (W5DMH log Jun 6 2026):
                //
                //   (1) The codec emitted verb=" MSG" with a leading
                //       space (visible as a double space between TO and
                //       VERB in the decoded text). strcmp(verb,"MSG")
                //       never matched, so the hook never fired at all.
                //       Fixed at the codec source (try_directed trims
                //       parts[2] leading whitespace).
                //
                //   (2) Even with (1) fixed, ACKing on the directed
                //       header frame would collide with the sender's
                //       continuation data frames. JS8 senders transmit
                //       all frames of a multi-frame MSG consecutively
                //       without listening between — they only listen
                //       for ACK after the LAST data frame. The fix
                //       below records the MSG header in a per-freq
                //       pending table; mailbox+ACK commit fires from
                //       the COMPLETE/PARTIAL_TAIL block further below
                //       when the assembler emits the joined body.
                //
                // Edge case — single-frame MSG (parts.size()==4 in
                // try_directed) with body in msg.body: commit
                // immediately, don't bother with pending. This is
                // rare in practice (real-world JS8 MSGs always seem
                // to use data-frame continuation) but the codec path
                // supports it.
                //
                // Edge case — own-callsign sender (DigiRig loopback):
                // ignore entirely. Operator chose this per L7.11g.4
                // decision answers.
                if (strcmp(msg.verb, "MSG") == 0) {
                    const nanojs8_config_t *cfg = nanojs8_config_get();
                    const char *my_call   = cfg ? cfg->callsign : "";
                    const char *my_groups = cfg ? cfg->groups   : "";

                    const bool from_self =
                        my_call[0] != '\0' &&
                        strcasecmp(msg.from_call, my_call) == 0;
                    const bool for_me =
                        nanojs8_activity_is_for_me(msg.to_call,
                                                    my_call,
                                                    my_groups);

                    if (from_self) {
                        ESP_LOGI(TAG,
                            "L7.11g.4 MSG header: ignoring "
                            "(from self %s, DigiRig loopback)",
                            my_call);
                    } else if (!for_me) {
                        ESP_LOGI(TAG,
                            "L7.11g.4 MSG header: ignoring "
                            "(not addressed to me: from=%s to=%s)",
                            msg.from_call, msg.to_call);
                    } else if (msg.body[0] != '\0') {
                        // Single-frame MSG with embedded body — commit now.
                        // L7.13: snr_db is the SNR of this body frame's decode.
                        const esp_err_t mb_err =
                            nanojs8_mailbox_add_unread(
                                msg.from_call,
                                msg.to_call,
                                msg.body,
                                (uint32_t)freq_hz_dec,
                                snr_db);
                        if (mb_err != ESP_OK) {
                            ESP_LOGW(TAG,
                                "L7.11g.4 MSG single-frame: "
                                "mailbox_add_unread failed "
                                "from=%s to=%s (%s)",
                                msg.from_call, msg.to_call,
                                esp_err_to_name(mb_err));
                        } else {
                            ESP_LOGI(TAG,
                                "L7.11g.4 MSG single-frame COMMITTED: "
                                "from=%s to=%s freq=%u body='%s'",
                                msg.from_call, msg.to_call,
                                (unsigned)freq_hz_dec, msg.body);
                        }
                        char ack_wire[NANOJS8_TX_QUEUE_WIRE_MAX];
                        snprintf(ack_wire, sizeof(ack_wire),
                                 "%s ACK", msg.from_call);
                        const esp_err_t qerr =
                            nanojs8_tx_queue_enqueue(ack_wire, "MSG-ack");
                        if (qerr != ESP_OK) {
                            ESP_LOGW(TAG,
                                "L7.11g.4 MSG single-frame: "
                                "ACK enqueue failed '%s' (%s)",
                                ack_wire, esp_err_to_name(qerr));
                        }
                    } else {
                        // Multi-frame header — defer commit.
                        msg_pending_record(msg.from_call, msg.to_call,
                                            (uint32_t)freq_hz_dec,
                                            esp_timer_get_time());
                    }
                }

                // L7.11g.6: QUERY MSGS verb (cmd id 12) — single-frame.
                //
                // The asker wants to know what STORE entries we hold
                // for them. Per JS8Call/MicroJS8 protocol:
                //   direct + hold   → "<asker> MSG <id>"  (oldest only)
                //   direct + empty  → "<asker> NO"
                //   @ALLCALL + hold → "<asker> MSG <id>"
                //   @ALLCALL + empty → silent (don't pollute band)
                //
                // is_for_me() excludes @ALLCALL by design (for MSG
                // verb privacy) so we OR an explicit @ALLCALL check.
                //
                // QUERY MSGS? variant (same cmd id 12 in directed_cmds)
                // gets the same handling.
                if (strcmp(msg.verb, "QUERY MSGS") == 0 ||
                    strcmp(msg.verb, "QUERY MSGS?") == 0) {
                    const nanojs8_config_t *cfg = nanojs8_config_get();
                    const char *my_call   = cfg ? cfg->callsign : "";
                    const char *my_groups = cfg ? cfg->groups   : "";

                    const bool from_self =
                        my_call[0] != '\0' &&
                        strcasecmp(msg.from_call, my_call) == 0;
                    // L7.11g.7-fix1: treat ANY @-prefix to_call as
                    // broadcast (was just @ALLCALL). Group QUERY MSGS
                    // arriving at multiple member stations would
                    // otherwise generate N "NO" replies all at once,
                    // jamming the channel. Only reply when we have
                    // something held; stay silent on empty for any
                    // broadcast (@ALLCALL or @GROUP).
                    const bool is_broadcast =
                        (msg.to_call[0] == '@');
                    const bool for_me_or_broadcast =
                        is_broadcast ||
                        nanojs8_activity_is_for_me(msg.to_call,
                                                    my_call, my_groups);

                    if (from_self) {
                        ESP_LOGI(TAG,
                            "L7.11g.6 QUERY MSGS: ignoring "
                            "(from self %s, loopback)", my_call);
                    } else if (msg.from_call[0] == '\0') {
                        // L7.11g.7-fix1: compound-from chaining miss.
                        // Without a sender we can't address a reply.
                        ESP_LOGW(TAG,
                            "L7.11g.7-fix1 QUERY MSGS: no from_call "
                            "(to=%s, compound chain may have missed) "
                            "— cannot reply",
                            msg.to_call);
                    } else if (!for_me_or_broadcast) {
                        ESP_LOGI(TAG,
                            "L7.11g.6 QUERY MSGS: ignoring "
                            "(not for me: from=%s to=%s)",
                            msg.from_call, msg.to_call);
                    } else {
                        // L7.11g.7-fix4: revert to single-reply
                        // (MicroJS8-spec) behavior. Per JS8 mailbox
                        // protocol, a QUERY MSGS reply is just the
                        // oldest held id; the asker then sends QUERY
                        // MSG <id> to retrieve text, ACKs, and may
                        // re-query for more. Single-reply is the
                        // wire-clean behavior every JS8 client
                        // expects. Earlier fix2 (freetext summary)
                        // hit an encoder hang; fix3 (multi-MSG burst)
                        // worked but was non-standard. Returning to
                        // the original L7.11g.6 design.
                        nanojs8_mailbox_entry_t held[1];
                        const uint32_t n =
                            nanojs8_mailbox_find_holding_for(
                                msg.from_call, held, 1);
                        char reply_wire[NANOJS8_TX_QUEUE_WIRE_MAX];
                        if (n > 0) {
                            // L7.11g.7-fix5: JS8Call convention is
                            // '<asker> YES MSG <id>' (per W5DMH's
                            // on-air protocol knowledge). The YES
                            // verb prefix tells stock JS8Call /
                            // MicroJS8 receivers this is a QUERY
                            // MSGS *response*, not an inbound MSG
                            // to be auto-ACKed and stored. Encoder
                            // packs as 2 frames: directed YES
                            // (verb id=27) plus a data continuation
                            // '" MSG <id>"'. YES is NOT auto-ACK-
                            // eligible, so the asker's UI shows our
                            // reply as an informational notification
                            // instead of attempting to store empty
                            // mail.
                            //
                            // The QUERY MSG <id> body-delivery path
                            // below DELIBERATELY does NOT use YES —
                            // there we DO want auto-ACK so the
                            // STORE→DELIVERED transition fires on
                            // the asker's ACK. Different wire
                            // semantics for different responses.
                            snprintf(reply_wire, sizeof(reply_wire),
                                     "%s YES MSG %u",
                                     msg.from_call,
                                     (unsigned)held[0].id);
                            ESP_LOGI(TAG,
                                "L7.11g.6 QUERY MSGS from %s "
                                "(to=%s): replying held id=%u "
                                "as 'YES MSG' (JS8Call conv)",
                                msg.from_call, msg.to_call,
                                (unsigned)held[0].id);
                            const esp_err_t qerr =
                                nanojs8_tx_queue_enqueue(reply_wire,
                                                         "QM-id");
                            if (qerr != ESP_OK) {
                                ESP_LOGW(TAG,
                                    "L7.11g.6 QUERY MSGS reply enq "
                                    "failed '%s' (%s)",
                                    reply_wire, esp_err_to_name(qerr));
                            }
                        } else if (!is_broadcast) {
                            // Direct-to-us + empty → informative NO.
                            snprintf(reply_wire, sizeof(reply_wire),
                                     "%s NO", msg.from_call);
                            ESP_LOGI(TAG,
                                "L7.11g.6 QUERY MSGS from %s: nothing "
                                "held, replying NO", msg.from_call);
                            const esp_err_t qerr =
                                nanojs8_tx_queue_enqueue(reply_wire,
                                                         "QM-no");
                            if (qerr != ESP_OK) {
                                ESP_LOGW(TAG,
                                    "L7.11g.6 QUERY MSGS NO enq "
                                    "failed (%s)",
                                    esp_err_to_name(qerr));
                            }
                        } else {
                            // Broadcast (@ALLCALL or @GROUP) + empty → silent.
                            ESP_LOGI(TAG,
                                "L7.11g.6 QUERY MSGS %s from %s: "
                                "nothing held, staying silent",
                                msg.to_call, msg.from_call);
                        }
                    }
                }

                // L7.11g.6: bare "QUERY" verb (cmd id 11) — multi-frame.
                //
                // Frame 1 carries verb="QUERY", body=empty. Subsequent
                // data frames carry the body (e.g. "MSG 73" for
                // QUERY MSG <id>). Defer dispatch until PARTIAL_TAIL/
                // COMPLETE assembles the body.
                if (strcmp(msg.verb, "QUERY") == 0) {
                    const nanojs8_config_t *cfg = nanojs8_config_get();
                    const char *my_call   = cfg ? cfg->callsign : "";
                    const char *my_groups = cfg ? cfg->groups   : "";

                    const bool from_self =
                        my_call[0] != '\0' &&
                        strcasecmp(msg.from_call, my_call) == 0;
                    const bool is_allcall =
                        strcasecmp(msg.to_call, "@ALLCALL") == 0;
                    const bool for_me_or_allcall =
                        is_allcall ||
                        nanojs8_activity_is_for_me(msg.to_call,
                                                    my_call, my_groups);

                    if (from_self) {
                        ESP_LOGI(TAG,
                            "L7.11g.6 QUERY header: ignoring (self)");
                    } else if (!for_me_or_allcall) {
                        ESP_LOGI(TAG,
                            "L7.11g.6 QUERY header: ignoring "
                            "(not for me: to=%s)", msg.to_call);
                    } else {
                        query_pending_record(msg.from_call, msg.to_call,
                                              (uint32_t)freq_hz_dec,
                                              esp_timer_get_time());
                    }
                }

                // L7.11g.7: "MSG TO:" verb (cmd id 10) — multi-frame relay.
                //
                // Frame 1 header has verb="MSG TO:"; data frames
                // carry "<FOR> <TEXT>". Defer dispatch to
                // PARTIAL_TAIL/COMPLETE assembler emission.
                //
                // We only accept relay requests directed at us (not
                // @ALLCALL — broadcasting a relay request to all
                // stations doesn't make sense, and would have every
                // listening station independently store the same
                // message, flooding the recipient with duplicates
                // when they eventually QUERY MSGS).
                if (strcmp(msg.verb, "MSG TO:") == 0) {
                    const nanojs8_config_t *cfg = nanojs8_config_get();
                    const char *my_call   = cfg ? cfg->callsign : "";
                    const char *my_groups = cfg ? cfg->groups   : "";

                    const bool from_self =
                        my_call[0] != '\0' &&
                        strcasecmp(msg.from_call, my_call) == 0;
                    const bool for_me =
                        nanojs8_activity_is_for_me(msg.to_call,
                                                    my_call, my_groups);

                    if (from_self) {
                        ESP_LOGI(TAG,
                            "L7.11g.7 MSG TO header: ignoring (self)");
                    } else if (!for_me) {
                        ESP_LOGI(TAG,
                            "L7.11g.7 MSG TO header: ignoring "
                            "(not for me: to=%s — relay request "
                            "addressed elsewhere)", msg.to_call);
                    } else {
                        msg_to_pending_record(msg.from_call, msg.to_call,
                                               (uint32_t)freq_hz_dec,
                                               esp_timer_get_time());
                    }
                }

                // L7.11g.6: ACK verb — single-frame.
                //
                // When a station ACKs us after we delivered a
                // "X MSG <id> <body>" via QUERY MSG <id>, transition
                // the STORE row to DELIVERED. Match by sender call
                // against our out_delivery tracker.
                //
                // Note: the very common case of an ACK to OUR ACK
                // (recipient acknowledging our auto-ACK on their MSG)
                // is filtered here too — those ACKs from senders we
                // don't have a pending delivery for are simply not
                // matched and the tracker stays empty.
                if (strcmp(msg.verb, "ACK") == 0) {
                    const nanojs8_config_t *cfg = nanojs8_config_get();
                    const char *my_call   = cfg ? cfg->callsign : "";
                    const char *my_groups = cfg ? cfg->groups   : "";

                    const bool from_self =
                        my_call[0] != '\0' &&
                        strcasecmp(msg.from_call, my_call) == 0;
                    const bool for_me =
                        nanojs8_activity_is_for_me(msg.to_call,
                                                    my_call, my_groups);

                    if (!from_self && for_me) {
                        uint16_t inbox_id = 0;
                        if (out_delivery_match_ack(msg.from_call,
                                                    esp_timer_get_time(),
                                                    &inbox_id)) {
                            const esp_err_t merr =
                                nanojs8_mailbox_mark_delivered(inbox_id);
                            if (merr == ESP_OK) {
                                ESP_LOGI(TAG,
                                    "L7.11g.6 ACK from %s: STORE id=%u "
                                    "transitioned to DELIVERED",
                                    msg.from_call, (unsigned)inbox_id);
                            } else {
                                ESP_LOGW(TAG,
                                    "L7.11g.6 ACK from %s: "
                                    "mark_delivered(%u) failed (%s)",
                                    msg.from_call, (unsigned)inbox_id,
                                    esp_err_to_name(merr));
                            }
                        }
                    }
                }

                // L7.11f-fix2f: when the msg_assembler emits a joined
                // multi-frame body (kind=PARTIAL_TAIL or COMPLETE), the
                // per-frame record_decode call above won't have stored
                // the data chunks (they have no from_call/verb). Feed
                // the assembler's full joined text back into the most
                // recent matching IN-entry's body via the continuation
                // API. SINGLE is skipped — the text is already a complete
                // single-frame message stored normally above.
                //
                // L7.11g.4-fix1: also commit any pending MSG at this
                // freq. The body is now fully assembled (asm_out.text),
                // the sender has stopped TXing, and the ACK can safely
                // go out in the next available slot.
                if (have_msg &&
                    (asm_out.kind == NANOJS8_JS8_ASM_KIND_PARTIAL_TAIL ||
                     asm_out.kind == NANOJS8_JS8_ASM_KIND_COMPLETE)) {
                    nanojs8_activity_set_body_continuation(
                        asm_out.freq_hz, asm_out.text);

                    msg_pending_t taken;
                    if (msg_pending_take((uint32_t)asm_out.freq_hz,
                                          esp_timer_get_time(), &taken)) {
                        // L7.13: snr_db here is the SNR of the body/tail
                        // frame's decode — the most recent measurement
                        // of this station, which is what the operator
                        // wants in the mailbox row.
                        const esp_err_t mb_err =
                            nanojs8_mailbox_add_unread(
                                taken.from_call,
                                taken.to_call,
                                asm_out.text,
                                taken.freq_hz,
                                snr_db);
                        if (mb_err != ESP_OK) {
                            ESP_LOGW(TAG,
                                "L7.11g.4 MSG commit: mailbox_add_unread "
                                "failed from=%s to=%s (%s)",
                                taken.from_call, taken.to_call,
                                esp_err_to_name(mb_err));
                        } else {
                            ESP_LOGI(TAG,
                                "L7.11g.4 MSG COMMITTED: "
                                "from=%s to=%s freq=%u body='%s' (kind=%s)",
                                taken.from_call, taken.to_call,
                                (unsigned)taken.freq_hz, asm_out.text,
                                asm_out.kind == NANOJS8_JS8_ASM_KIND_COMPLETE
                                    ? "COMPLETE" : "PARTIAL_TAIL");
                        }

                        char ack_wire[NANOJS8_TX_QUEUE_WIRE_MAX];
                        snprintf(ack_wire, sizeof(ack_wire),
                                 "%s ACK", taken.from_call);
                        const esp_err_t qerr =
                            nanojs8_tx_queue_enqueue(ack_wire, "MSG-ack");
                        if (qerr != ESP_OK) {
                            ESP_LOGW(TAG,
                                "L7.11g.4 MSG commit: ACK enqueue "
                                "failed '%s' (%s)",
                                ack_wire, esp_err_to_name(qerr));
                        }
                    }

                    // L7.11g.6: QUERY MSG <id> dispatch.
                    //
                    // The QUERY header frame recorded the asker into
                    // s_query_pending. Now the body has arrived in
                    // asm_out.text. Parse "MSG <id>" and respond with
                    // the matching STORE entry's body.
                    query_pending_t qp;
                    if (query_pending_take((uint32_t)asm_out.freq_hz,
                                            esp_timer_get_time(), &qp)) {
                        const uint16_t requested_id =
                            parse_query_msg_id(asm_out.text);
                        if (requested_id == 0) {
                            ESP_LOGI(TAG,
                                "L7.11g.6 QUERY body from %s: "
                                "unrecognized '%s' — ignoring",
                                qp.asker_call, asm_out.text);
                        } else {
                            nanojs8_mailbox_entry_t row;
                            const bool found =
                                nanojs8_mailbox_find_by_id(requested_id,
                                                            &row);
                            if (!found) {
                                ESP_LOGI(TAG,
                                    "L7.11g.6 QUERY MSG %u from %s: "
                                    "id not found in mailbox",
                                    (unsigned)requested_id,
                                    qp.asker_call);
                            } else if (row.type !=
                                       NANOJS8_MAILBOX_TYPE_STORE) {
                                ESP_LOGI(TAG,
                                    "L7.11g.6 QUERY MSG %u from %s: "
                                    "row type=%u (not STORE), refusing",
                                    (unsigned)requested_id,
                                    qp.asker_call, (unsigned)row.type);
                            } else if (strcasecmp(row.to_call,
                                                  qp.asker_call) != 0) {
                                ESP_LOGI(TAG,
                                    "L7.11g.6 QUERY MSG %u from %s: "
                                    "row TO=%s, not for asker — refusing",
                                    (unsigned)requested_id,
                                    qp.asker_call, row.to_call);
                            } else {
                                // L7.11g.7-fix1: for relay rows
                                // (from_call != our_callsign) prepend
                                // "DE <originator>" to the body so
                                // the recipient sees who originally
                                // composed the message, not just the
                                // relay station they queried. Local
                                // stores (from_call == us) deliver
                                // bare body — the recipient already
                                // knows we sent it because we're the
                                // wire FROM.
                                const nanojs8_config_t *cfg =
                                    nanojs8_config_get();
                                const char *my_call =
                                    cfg ? cfg->callsign : "";
                                const bool is_relay =
                                    my_call[0] != '\0' &&
                                    strcasecmp(row.from_call,
                                                my_call) != 0;

                                char body_wire[NANOJS8_TX_QUEUE_WIRE_MAX];

                                /*
                                 * L7.11g.7-fix6: sanitize body free-text
                                 * before wire pack. JS8 Huffman codebook is
                                 * 44 chars only (space, A-Z, 0-9, .-+?!"/)
                                 * and JSC::compress is stubbed for flash
                                 * savings — meaning packHuffMessage is our
                                 * only data-frame path and it's strictly
                                 * all-or-nothing on alphabet membership.
                                 *
                                 * Without this sanitization step, ANY
                                 * single invalid char in row.body (e.g. a
                                 * '#' or ',') causes packDataMessage to
                                 * return 0 chars packed, which trips the
                                 * fix3 safety guard in buildMessageFrames,
                                 * which means only the directed header
                                 * frame goes out. The recipient's JS8Call
                                 * decodes that as "MSG <id>" with empty
                                 * body and auto-ACKs as a ping. We then
                                 * mark STORE→DELIVERED off the false ACK
                                 * while the body never reached the air.
                                 *
                                 * On-air Jun 7 2026 (L7.11g.7-fix5): id=9
                                 * body "MULTI MSG TES #1" — '#' killed the
                                 * data continuation, KD8PGB auto-ACK'd, we
                                 * marked DELIVERED, body lost. See
                                 * js8_freetext.h for the full chain.
                                 *
                                 * Operator preference Jun 7 2026: replace
                                 * invalid chars with SPACE (silent gap in
                                 * recipient's display). Mailbox NVS keeps
                                 * the operator's original bytes — only
                                 * the wire copy is sanitized — so INBOX
                                 * still shows '#' / ',' / etc. faithfully.
                                 */
                                char body_san[NANOJS8_MAILBOX_BODY_LEN + 1];
                                strncpy(body_san, row.body,
                                        sizeof(body_san) - 1);
                                body_san[sizeof(body_san) - 1] = '\0';
                                const size_t san_changed =
                                    nanojs8_js8_freetext_sanitize(body_san);
                                if (san_changed > 0) {
                                    ESP_LOGW(TAG,
                                        "L7.11g.7-fix6 body sanitize: "
                                        "id=%u %zu char(s) outside JS8 "
                                        "Huffman alphabet replaced with "
                                        "space — wire='%s' (was='%s')",
                                        (unsigned)row.id, san_changed,
                                        body_san, row.body);
                                }

                                if (is_relay) {
                                    snprintf(body_wire, sizeof(body_wire),
                                             "%s MSG %u DE %s %s",
                                             qp.asker_call,
                                             (unsigned)row.id,
                                             row.from_call, body_san);
                                } else {
                                    snprintf(body_wire, sizeof(body_wire),
                                             "%s MSG %u %s",
                                             qp.asker_call,
                                             (unsigned)row.id, body_san);
                                }
                                ESP_LOGI(TAG,
                                    "L7.11g.6 QUERY MSG %u from %s: "
                                    "delivering body (%zu chars) "
                                    "%s",
                                    (unsigned)row.id, qp.asker_call,
                                    strlen(row.body),
                                    is_relay ? "[relay — DE origin "
                                                "prepended]"
                                              : "[local store]");
                                const esp_err_t qerr =
                                    nanojs8_tx_queue_enqueue(body_wire,
                                                             "QM-body");
                                if (qerr != ESP_OK) {
                                    ESP_LOGW(TAG,
                                        "L7.11g.6 QUERY MSG body enq "
                                        "failed '%s' (%s)",
                                        body_wire, esp_err_to_name(qerr));
                                } else {
                                    // Track for STORE→DELIVERED on ACK.
                                    out_delivery_record(qp.asker_call,
                                                         row.id,
                                                         esp_timer_get_time());
                                }
                            }
                        }
                    }

                    // L7.11g.7: MSG TO: dispatch.
                    //
                    // Header recorded the relay request; body has
                    // arrived as "<FOR> <TEXT>". Parse, validate,
                    // store as STORE for FOR, and ACK the originator.
                    msg_to_pending_t mtop;
                    if (msg_to_pending_take((uint32_t)asm_out.freq_hz,
                                              esp_timer_get_time(), &mtop)) {
                        char for_buf[NANOJS8_MAILBOX_CALL_LEN];
                        const char *text =
                            parse_msg_to_body(asm_out.text,
                                                for_buf, sizeof(for_buf));
                        if (!text) {
                            ESP_LOGI(TAG,
                                "L7.11g.7 MSG TO body from %s: "
                                "unparseable '%s' — refusing",
                                mtop.sender_call, asm_out.text);
                        } else {
                            // Reject relay TO ourselves (sender should
                            // use plain MSG) and TO groups/broadcasts.
                            const nanojs8_config_t *cfg = nanojs8_config_get();
                            const char *my_call = cfg ? cfg->callsign : "";
                            const bool to_self =
                                my_call[0] != '\0' &&
                                strcasecmp(for_buf, my_call) == 0;
                            const bool to_group = (for_buf[0] == '@');

                            if (to_self) {
                                ESP_LOGI(TAG,
                                    "L7.11g.7 MSG TO:%s from %s — "
                                    "recipient is us; refusing relay "
                                    "(sender should use plain MSG)",
                                    for_buf, mtop.sender_call);
                            } else if (to_group) {
                                ESP_LOGI(TAG,
                                    "L7.11g.7 MSG TO:%s from %s — "
                                    "group/broadcast relay refused",
                                    for_buf, mtop.sender_call);
                            } else if (strlen(for_buf) < 3) {
                                ESP_LOGI(TAG,
                                    "L7.11g.7 MSG TO body from %s: "
                                    "FOR='%s' too short — refusing",
                                    mtop.sender_call, for_buf);
                            } else {
                                const esp_err_t aerr =
                                    nanojs8_mailbox_add_store(
                                        mtop.sender_call, for_buf, text);
                                if (aerr != ESP_OK) {
                                    ESP_LOGW(TAG,
                                        "L7.11g.7 MSG TO add_store "
                                        "failed (from=%s for=%s): %s",
                                        mtop.sender_call, for_buf,
                                        esp_err_to_name(aerr));
                                } else {
                                    ESP_LOGI(TAG,
                                        "L7.11g.7 MSG TO accepted: "
                                        "from=%s for=%s text=(%zu chars) — "
                                        "stored as STORE for relay",
                                        mtop.sender_call, for_buf,
                                        strlen(text));
                                    // ACK the originator so they know
                                    // we accepted the relay.
                                    char ack_wire[NANOJS8_TX_QUEUE_WIRE_MAX];
                                    snprintf(ack_wire, sizeof(ack_wire),
                                             "%s ACK", mtop.sender_call);
                                    const esp_err_t qerr =
                                        nanojs8_tx_queue_enqueue(
                                            ack_wire, "MTO-ack");
                                    if (qerr != ESP_OK) {
                                        ESP_LOGW(TAG,
                                            "L7.11g.7 MSG TO ACK enq "
                                            "failed '%s' (%s)",
                                            ack_wire,
                                            esp_err_to_name(qerr));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        const uint32_t cpu_ms =
            (uint32_t)((esp_timer_get_time() - t0) / 1000);

        // Find the best-score candidate for stats. n_cand may be 0.
        int16_t best_score = 0;
        for (int i = 0; i < n_cand; ++i) {
            if (s_cand_heap[i].score > best_score) {
                best_score = s_cand_heap[i].score;
            }
        }

        // Publish stats (atomics).
        atomic_fetch_add(&g_stats.slots_processed, 1);
        atomic_store(&g_stats.last_slot_candidates, (uint32_t)n_cand);
        atomic_fetch_add(&g_stats.total_candidates, (uint32_t)n_cand);
        atomic_store(&g_stats.last_slot_best_score, (int)best_score);
        atomic_store(&g_stats.last_slot_cpu_ms, cpu_ms);
        // L7.6: decode-pipeline stats.
        atomic_store(&g_stats.last_slot_attempts, slot_attempts);
        atomic_store(&g_stats.last_slot_decodes,  slot_decodes);
        atomic_fetch_add(&g_stats.total_decodes,  slot_decodes);

        // Stack hwm tracking.
        const uint32_t hwm_bytes =
            uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        uint32_t prev = atomic_load(&g_stats.stack_min_free);
        while (hwm_bytes < prev) {
            if (atomic_compare_exchange_weak(
                    &g_stats.stack_min_free, &prev, hwm_bytes)) {
                break;
            }
        }

        // Build UTC HH:MM:SS prefix from the slot's UTC second-of-day.
        const uint32_t sod = rxs.last_slot_seconds_today;
        const unsigned hh = (sod / 3600) % 24;
        const unsigned mm = (sod / 60)   % 60;
        const unsigned ss =  sod          % 60;

        // Per-slot summary — one line even if no candidates, so
        // operators see the heartbeat of the sync engine on a dead band.
        // L7.6: includes decode-attempt + decode-success counters.
        ESP_LOGI(TAG, "Slot %02u:%02u:%02u: %d candidate%s, %u decoded "
                      "(best score=%d, peak audio=%d, cpu=%" PRIu32 " ms, "
                      "stack_hwm=%" PRIu32 " B)",
                 hh, mm, ss,
                 n_cand, (n_cand == 1) ? "" : "s",
                 (unsigned)slot_decodes,
                 (int)best_score, (int)rxs.last_slot_peak,
                 cpu_ms, hwm_bytes);

        // Detail lines: log each candidate with its parameters. Frequency
        // = freq_offset bins × (sample_rate / nfft) Hz. With nfft=3840,
        // freq_osr=2, the effective resolution is (12000 / 3840) Hz per
        // bin = 3.125 Hz.
        const float bin_hz =
            (float)JS8_SYNC_SAMPLE_RATE / (float)s_mon.nfft;
        for (int i = 0; i < n_cand; ++i) {
            const ftx_candidate_t *c = &s_cand_heap[i];
            const float freq_hz =
                ((float)c->freq_offset + (float)c->freq_sub /
                                            (float)JS8_SYNC_FREQ_OSR) * bin_hz
                + JS8_SYNC_F_MIN /* min_bin offset is rolled into find */;
            const float dt_s =
                ((float)c->time_offset + (float)c->time_sub /
                                            (float)JS8_SYNC_TIME_OSR) *
                (1.0f / 6.25f);   // symbol_period_s for FT8/JS8 Normal
            ESP_LOGI(TAG, "  cand #%d: score=%d freq=%5.1f Hz dt=%+5.2f s",
                     i, (int)c->score, (double)freq_hz, (double)dt_s);
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

esp_err_t nanojs8_js8_sync_start(void)
{
    // Stack size: 24 KB. Per-slot work is bounded (one ftx_find_candidates
    // + one monitor sweep). ft8_lib has no LDPC chain here (that's L7.6),
    // so peak stack is mostly the chunk buffer reads + small locals.
    // Generous initial budget; uxTaskGetStackHighWaterMark in the loop
    // will let us trim later.
    //
    // L7.14-fix7: stack allocated in PSRAM via xTaskCreatePinnedToCoreWithCaps.
    // Previously this used xTaskCreatePinnedToCore which pulls from internal
    // RAM. After the L7.14-fix4 incident — where an 8 KB int16 LUT moved
    // into BSS dropped the largest internal contig from 31744 B to 23552 B
    // and broke this allocation (ESP_ERR_NO_MEM at boot, JS8 RX dead) —
    // it's clear internal RAM headroom is the long-tail risk. PSRAM has
    // ~5 MB free at start-of-sync and zero competition for big blocks.
    // The 64 KB PSRAM data cache covers any tight inner loops; we already
    // hold the snapshot buffer (351 KB) in PSRAM with no observable slow-
    // down. Matching cleanup is vTaskDeleteWithCaps inside sync_task.
    //
    // NOTE: WithCaps takes stack size in BYTES (not words like the
    // non-Caps variant). Easy to get wrong; passing words would
    // under-allocate by 4×.
    const uint32_t kStackBytes = 24 * 1024;

    // Diagnostic capture before the create — if PSRAM is somehow exhausted
    // we'll have the numbers in the log instead of a bare ESP_ERR_NO_MEM.
    ESP_LOGI(TAG,
        "Pre-create heap: internal free=%u largest=%u, "
        "PSRAM free=%u largest=%u; requesting %u-byte stack in PSRAM",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)kStackBytes);

    TaskHandle_t h = NULL;
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(
        sync_task, "js8sync", kStackBytes, NULL,
        /* prio */ 5, &h, /* core */ 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) {
        ESP_LOGE(TAG,
            "xTaskCreatePinnedToCoreWithCaps failed (rc=%d, likely PSRAM OOM)",
            (int)r);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void nanojs8_js8_sync_get_stats(nanojs8_js8_sync_stats_t *out)
{
    if (!out) return;
    out->slots_processed     = atomic_load(&g_stats.slots_processed);
    out->last_slot_candidates = atomic_load(&g_stats.last_slot_candidates);
    out->total_candidates    = atomic_load(&g_stats.total_candidates);
    out->last_slot_best_score =
        (int16_t)atomic_load(&g_stats.last_slot_best_score);
    out->last_slot_cpu_ms    = atomic_load(&g_stats.last_slot_cpu_ms);
    out->stack_min_free      = atomic_load(&g_stats.stack_min_free);
    // L7.6
    out->last_slot_attempts  = atomic_load(&g_stats.last_slot_attempts);
    out->last_slot_decodes   = atomic_load(&g_stats.last_slot_decodes);
    out->total_decodes       = atomic_load(&g_stats.total_decodes);
}
