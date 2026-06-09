/*
 * msg_assembler.c — L7.8 multi-frame JS8 message assembly
 * =============================================================================
 * JS8 messages longer than ~13 characters span multiple 15-second frames.
 * The TransmissionType bits encode each frame's position in the sequence:
 *
 *   bit 0 (FIRST = 1)  set on the first frame of a multi-frame message
 *   bit 1 (LAST  = 2)  set on the last frame of a multi-frame message
 *   bit 2 (DATA  = 4)  set on raw data frames (no frame-type header inside)
 *
 *   FIRST|LAST = 3  →  single-frame message (both flags on same frame)
 *
 * The assembler maintains a tiny static table of in-flight buffers indexed
 * by audio frequency. When a FIRST is seen, a buffer is opened. Middle
 * frames append. LAST emits the assembled text and clears the buffer.
 *
 * Real-world wrinkles handled:
 *   1. We tuned in mid-stream — middle/LAST arrives without a FIRST.
 *      Strategy: open a buffer anyway, emit on LAST with kind=PARTIAL_TAIL
 *      so the operator knows it's not the full message.
 *   2. Frequency drift between frames. ±25 Hz lookup tolerance.
 *   3. Lost FIRST or LAST (band fade). Timeout (90 s) GC's stale buffers
 *      before they can collide with the next QSO at that frequency.
 *   4. Same frame decoded multiple times (per-slot candidate dupes).
 *      Caller dedupes in js8_sync.c BEFORE invoking us, so this layer
 *      assumes one frame per call.
 *
 * Single-thread design — invoked only from the js8_sync task. No locking.
 *
 * License: GPL-3.0
 */

#include "js8_codec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>   // PRIu32 (used in ESP_LOGW format strings)

#include "esp_log.h"

static const char *TAG = "js8asm";

// ── Internal slot record ─────────────────────────────────────────────────────
//
// One in-flight assembly. The text buffer is sized to NANOJS8_JS8_ASM_TEXT_MAX
// (512 chars) which covers all realistic JS8 multi-frame messages.

typedef struct {
    bool      in_use;
    bool      saw_first;       // distinguishes COMPLETE vs PARTIAL_TAIL at emit
    float     freq_hz;
    uint16_t  frame_count;
    uint16_t  text_len;        // bytes currently in `text`, not counting NUL
    uint32_t  last_update_ms;
    char      text[NANOJS8_JS8_ASM_TEXT_MAX];
} asm_slot_t;

static asm_slot_t s_slots[NANOJS8_JS8_ASM_MAX_SLOTS];
static _Atomic uint32_t s_active_count = 0;

// ── Slot helpers ─────────────────────────────────────────────────────────────

/// Return true if a frame at `freq` matches this slot's stored frequency.
static inline bool freq_matches(const asm_slot_t *slot, float freq)
{
    const float d = freq - slot->freq_hz;
    return (d >= -NANOJS8_JS8_ASM_FREQ_TOL_HZ &&
            d <=  NANOJS8_JS8_ASM_FREQ_TOL_HZ);
}

/// Linear search — N=8 makes this trivially fast.
static asm_slot_t *find_slot(float freq)
{
    for (int i = 0; i < NANOJS8_JS8_ASM_MAX_SLOTS; ++i) {
        if (s_slots[i].in_use && freq_matches(&s_slots[i], freq))
            return &s_slots[i];
    }
    return NULL;
}

/// Find an idle slot, or evict the oldest slot if all are in use.
/// We prefer evicting an inactive (old) slot over interrupting an active
/// one. With N=8 slots and 90-second timeout, eviction should be rare.
static asm_slot_t *alloc_slot(uint32_t now_ms)
{
    asm_slot_t *first_free = NULL;
    asm_slot_t *oldest     = &s_slots[0];

    for (int i = 0; i < NANOJS8_JS8_ASM_MAX_SLOTS; ++i) {
        if (!s_slots[i].in_use) {
            if (!first_free) first_free = &s_slots[i];
        } else {
            if (s_slots[i].last_update_ms < oldest->last_update_ms) {
                oldest = &s_slots[i];
            }
        }
    }

    if (first_free) return first_free;

    // All occupied — evict the oldest. Log it so operators see why a
    // buffer disappeared.
    ESP_LOGW(TAG, "all %d slots in use — evicting freq=%.1f Hz "
                  "(%u frames, idle %" PRIu32 " ms)",
             NANOJS8_JS8_ASM_MAX_SLOTS,
             (double)oldest->freq_hz,
             (unsigned)oldest->frame_count,
             now_ms - oldest->last_update_ms);
    return oldest;
}

/// Open a fresh slot at this frequency, clearing any prior content.
static void slot_open(asm_slot_t *slot, float freq, bool saw_first,
                      uint32_t now_ms)
{
    slot->in_use         = true;
    slot->saw_first      = saw_first;
    slot->freq_hz        = freq;
    slot->frame_count    = 0;
    slot->text_len       = 0;
    slot->text[0]        = '\0';
    slot->last_update_ms = now_ms;
}

/// Mark slot empty.
static void slot_clear(asm_slot_t *slot)
{
    slot->in_use      = false;
    slot->saw_first   = false;
    slot->freq_hz     = 0.0f;
    slot->frame_count = 0;
    slot->text_len    = 0;
    slot->text[0]     = '\0';
}

/// Append text to a slot. Truncates if it would overflow — we cap at
/// NANOJS8_JS8_ASM_TEXT_MAX - 1 chars and keep the slot in_use so further
/// frames still update last_update_ms (preventing the slot's premature GC
/// from beating its own LAST frame).
static void slot_append(asm_slot_t *slot, const char *frame_text)
{
    if (!frame_text) return;
    const size_t add  = strnlen(frame_text, NANOJS8_JS8_ASM_TEXT_MAX);
    const size_t cap  = (size_t)NANOJS8_JS8_ASM_TEXT_MAX - 1;
    const size_t room = (slot->text_len < cap) ? (cap - slot->text_len) : 0;
    const size_t n    = (add < room) ? add : room;

    if (n > 0) {
        memcpy(slot->text + slot->text_len, frame_text, n);
        slot->text_len += (uint16_t)n;
        slot->text[slot->text_len] = '\0';
    }
    slot->frame_count += 1;
}

/// Sweep slots, freeing any older than the timeout.
static void slot_gc(uint32_t now_ms)
{
    uint32_t active = 0;
    for (int i = 0; i < NANOJS8_JS8_ASM_MAX_SLOTS; ++i) {
        if (!s_slots[i].in_use) continue;
        // Use signed delta to handle 32-bit wrap. Should not wrap during a
        // run (49.7 days @ 1ms), but be defensive.
        const uint32_t age = now_ms - s_slots[i].last_update_ms;
        if (age >= NANOJS8_JS8_ASM_TIMEOUT_MS) {
            ESP_LOGD(TAG, "GC freq=%.1f Hz (%u frames, idle %" PRIu32 " ms)",
                     (double)s_slots[i].freq_hz,
                     (unsigned)s_slots[i].frame_count, age);
            slot_clear(&s_slots[i]);
        } else {
            ++active;
        }
    }
    atomic_store(&s_active_count, active);
}

// ── Public API ───────────────────────────────────────────────────────────────

void nanojs8_js8_assembler_reset(void)
{
    for (int i = 0; i < NANOJS8_JS8_ASM_MAX_SLOTS; ++i) {
        slot_clear(&s_slots[i]);
    }
    atomic_store(&s_active_count, 0);
}

uint32_t nanojs8_js8_assembler_active_count(void)
{
    return atomic_load(&s_active_count);
}

bool nanojs8_js8_assemble_frame(float freq_hz,
                                 uint8_t tx_type,
                                 uint8_t frame_subtype,
                                 const char *frame_text,
                                 uint32_t now_ms,
                                 nanojs8_js8_assembly_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->kind = NANOJS8_JS8_ASM_KIND_NONE;

    if (!frame_text) frame_text = "";

    const bool is_first =
        (tx_type & NANOJS8_JS8_TX_FIRST) == NANOJS8_JS8_TX_FIRST;
    const bool is_last =
        (tx_type & NANOJS8_JS8_TX_LAST)  == NANOJS8_JS8_TX_LAST;

    // Only data-class frames participate in multi-frame assembly. Heartbeats,
    // single-frame directed, and compound frames are emitted as-is. Caller
    // can still log them through their normal path; we just don't buffer.
    const bool is_data =
        (frame_subtype == NANOJS8_JS8_FRAME_DATA) ||
        (frame_subtype == NANOJS8_JS8_FRAME_DATA_COMPRESSED);

    if (!is_data) {
        // Non-data: pass through as SINGLE_FRAME so the caller can choose
        // a uniform display path for "complete" messages.
        out->kind        = NANOJS8_JS8_ASM_KIND_SINGLE_FRAME;
        out->freq_hz     = freq_hz;
        out->frame_count = 1;
        const size_t n   = strnlen(frame_text, sizeof(out->text) - 1);
        memcpy(out->text, frame_text, n);
        out->text[n] = '\0';
        return true;
    }

    // Opportunistic GC at every call. Cheap (N=8 scan), keeps stale
    // buffers from haunting us when a new QSO opens at a fresh freq.
    slot_gc(now_ms);

    // Case 1: FIRST and LAST both set → single-frame DATA. Emit
    // immediately without touching the slot table.
    if (is_first && is_last) {
        out->kind        = NANOJS8_JS8_ASM_KIND_SINGLE_FRAME;
        out->freq_hz     = freq_hz;
        out->frame_count = 1;
        const size_t n   = strnlen(frame_text, sizeof(out->text) - 1);
        memcpy(out->text, frame_text, n);
        out->text[n] = '\0';
        return true;
    }

    // Case 2: FIRST without LAST → open a fresh buffer. If a buffer is
    // already in flight at this frequency, replace it (the prior message
    // is presumed abandoned, since a real FIRST means a new transmission).
    if (is_first && !is_last) {
        asm_slot_t *slot = find_slot(freq_hz);
        if (!slot) slot = alloc_slot(now_ms);
        slot_open(slot, freq_hz, /*saw_first=*/true, now_ms);
        slot_append(slot, frame_text);
        slot->last_update_ms = now_ms;
        atomic_fetch_add(&s_active_count, 1);
        // No emit — still in progress.
        return false;
    }

    // Case 3: LAST without FIRST → close out the buffer at this freq.
    // If no buffer exists, this is a partial tail (we joined mid-stream
    // or lost the FIRST). Emit what we have either way.
    if (is_last && !is_first) {
        asm_slot_t *slot = find_slot(freq_hz);
        const bool had_buffer = (slot != NULL);
        if (!slot) {
            // No prior buffer. Allocate transient storage in `out` directly.
            out->kind        = NANOJS8_JS8_ASM_KIND_PARTIAL_TAIL;
            out->freq_hz     = freq_hz;
            out->frame_count = 1;
            const size_t n   = strnlen(frame_text, sizeof(out->text) - 1);
            memcpy(out->text, frame_text, n);
            out->text[n] = '\0';
            return true;
        }
        slot_append(slot, frame_text);
        out->kind        = slot->saw_first ?
                           NANOJS8_JS8_ASM_KIND_COMPLETE :
                           NANOJS8_JS8_ASM_KIND_PARTIAL_TAIL;
        out->freq_hz     = slot->freq_hz;
        out->frame_count = slot->frame_count;
        const size_t n   = (slot->text_len < sizeof(out->text) - 1)
                           ? slot->text_len
                           : sizeof(out->text) - 1;
        memcpy(out->text, slot->text, n);
        out->text[n] = '\0';
        slot_clear(slot);
        atomic_fetch_sub(&s_active_count, 1);
        (void)had_buffer;
        return true;
    }

    // Case 4: middle frame (no FIRST, no LAST). Append to an existing
    // slot at this freq, or open a new "partial-start" slot if we don't
    // have one yet (we joined mid-stream).
    asm_slot_t *slot = find_slot(freq_hz);
    if (!slot) {
        slot = alloc_slot(now_ms);
        slot_open(slot, freq_hz, /*saw_first=*/false, now_ms);
        atomic_fetch_add(&s_active_count, 1);
    }
    slot_append(slot, frame_text);
    slot->last_update_ms = now_ms;
    return false;
}
