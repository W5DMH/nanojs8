/*
 * tx_queue.h — NanoJS8 v0.7 L7.11g.4 TX queue
 * =================================================================
 *
 * Short-form-reply TX queue. Sits in front of tx_audio's single-flight
 * transmit_text() and absorbs back-to-back enqueue calls from RX
 * detection paths (auto-ACK on MSG verbs, future QUERY MSGS replies)
 * so they don't get lost when an existing TX is still in flight or
 * when multiple stations ping us in the same slot pair.
 *
 * Lifetime: spawned at boot. Drain task wakes on enqueue, waits for
 * tx_audio_is_active() to clear, then calls transmit_text(). One
 * frame per loop iteration so the task yields between transmissions
 * and re-checks state.
 *
 * Sizing: 4 entries × (128 B wire + 24 B reason + 8 B timestamp) ≈
 * 640 B BSS. 4 is the largest depth a real-world JS8 slot pair could
 * plausibly produce (one MSG from each of ~4 nearby stations in the
 * same 15-s window). Beyond that we drop the oldest with a WARN —
 * the most recent ACK obligations win.
 *
 * Thread safety: enqueue() is multi-producer safe via mutex. The
 * drain task is the sole consumer. Stats are atomic_uint for
 * lock-free observability.
 *
 * License: GPL-3.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Sizing ───────────────────────────────────────────────────────────

// 4 entries — covers a busy QSO with multiple stations MSG'ing us
// back-to-back in the same slot pair. Beyond this we drop oldest.
#define NANOJS8_TX_QUEUE_DEPTH       4

// Max wire length. Matches tx_audio's per-call buffer convention; a
// queued reply should be short ("CALL ACK", "CALL ACK MSG id",
// "CALL MSG body"). Anything longer should be composed via the
// COMPOSE screen which spawns its own TX directly.
// L7.11g.7-fix1: bumped from 128 → 192 to accommodate the relay-
// origin wire format "<asker> MSG <id> DE <originator> <body>"
// (worst case: 15 + 5 + 10 + 4 + 15 + 1 + 99 + 1 = 150 B). Older
// formats fit easily inside the new size. Cost: +64 B per queue
// entry × 4 entries = +256 B BSS in tx_queue, plus +64 B per
// transient wire buffer on the sync_task stack.
#define NANOJS8_TX_QUEUE_WIRE_MAX    192

// Reason tag length — short label for the heartbeat log so the
// operator can see why a queued TX is pending.
#define NANOJS8_TX_QUEUE_REASON_MAX  24

// ── Stats ────────────────────────────────────────────────────────────

typedef struct {
    uint32_t depth;                    // entries currently held
    uint32_t total_enqueued;           // monotonic since boot
    uint32_t total_transmitted;        // ok returns from transmit_text
    uint32_t total_dropped_overflow;   // pre-empted by a newer enqueue
    uint32_t total_dropped_tx_err;     // transmit_text returned non-OK
} nanojs8_tx_queue_stats_t;

// ── API ──────────────────────────────────────────────────────────────

// Initialize the queue and spawn the drain task. Idempotent. Returns
// ESP_OK on success; ESP_FAIL if mutex/task creation fails.
//
// MUST be called AFTER tx_audio's init+self-test so the drain task
// can call transmit_text() against a working audio path.
esp_err_t nanojs8_tx_queue_init(void);

// Enqueue a wire string for transmission. `reason` is a short tag
// shown in logs ("auto-ACK", "QUERY MSGS reply", etc.) — pass NULL or
// "" if you don't care. The wire is copied; caller doesn't retain it.
//
// Returns ESP_OK on enqueue; ESP_ERR_INVALID_ARG for empty wire;
// ESP_ERR_INVALID_STATE if init hasn't run; ESP_FAIL on mutex timeout.
// When the queue is full, the OLDEST entry is dropped (logged WARN)
// to make room — the call still returns ESP_OK.
esp_err_t nanojs8_tx_queue_enqueue(const char *wire, const char *reason);

// Current queue depth. Atomic read; no mutex. Safe for heartbeat use.
uint32_t  nanojs8_tx_queue_count(void);

// Stats snapshot for heartbeat / diagnostics. Atomic reads.
void      nanojs8_tx_queue_get_stats(nanojs8_tx_queue_stats_t *out);

#ifdef __cplusplus
}
#endif
