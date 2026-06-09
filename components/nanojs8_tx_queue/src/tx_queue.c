/*
 * tx_queue.c — NanoJS8 v0.7 L7.11g.4 TX queue implementation
 * ============================================================
 *
 * See tx_queue.h for the API contract.
 *
 * Internal design:
 *
 *   Ring buffer of NANOJS8_TX_QUEUE_DEPTH entries, indexed by head
 *   (next-read) and tail (next-write). Mutex-protected for the
 *   producer side (multiple RX paths may enqueue concurrently). The
 *   drain task is the SOLE consumer.
 *
 *   Drain task lifecycle:
 *
 *     while (true):
 *       wait for signal OR timeout 500 ms (re-check periodically)
 *       wait for tx_audio_is_active() to clear
 *       pop head under mutex
 *       call transmit_text(wire)
 *       loop
 *
 *   The periodic timeout lets the task recover from missed signals
 *   (rare — the binary semaphore should be reliable, but the 500 ms
 *   poll is cheap insurance).
 *
 *   Overflow policy: drop OLDEST when enqueueing into a full ring.
 *   ACK obligations are time-sensitive — the most recent MSG is the
 *   one whose sender is still listening for our reply. An older
 *   pending ACK that's been queued for 60+ seconds may already have
 *   timed out on the other end.
 *
 *   Drain task is pinned to CPU 0 so it doesn't compete with the
 *   js8_sync decoder + tx_audio worker which both pin to CPU 1.
 *   CPU 0 hosts the UI render loop + USB host but has cycles to
 *   spare for the occasional transmit_text marshaling.
 *
 *   Stack: 16 KB PSRAM. transmit_text runs a synchronous
 *   nanojs8_js8_text_frame_count() pre-flight (L7.14-fix9) that
 *   triggers std::regex via Varicode::pack on the caller's stack —
 *   journal sizing for that chain is 14-20 KB. L7.16-fix2 bumped
 *   this stack up to fit. Detailed rationale at the task-create site.
 *
 * License: GPL-3.0
 */

#include "tx_queue.h"

#include "esp_err.h"
#include "esp_heap_caps.h"            // L7.16-fix2: MALLOC_CAP_SPIRAM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"   // L7.16-fix2: xTaskCreatePinnedToCoreWithCaps
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tx_audio.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "tx_queue";

// ── Ring entry ───────────────────────────────────────────────────────

typedef struct {
    char     wire[NANOJS8_TX_QUEUE_WIRE_MAX];
    char     reason[NANOJS8_TX_QUEUE_REASON_MAX];
    int64_t  at_us;                                    // enqueue time
} tx_queue_entry_t;

// ── State (BSS) ──────────────────────────────────────────────────────

static tx_queue_entry_t  s_entries[NANOJS8_TX_QUEUE_DEPTH];
static uint32_t          s_head;
static uint32_t          s_tail;
static uint32_t          s_count;            // 0..NANOJS8_TX_QUEUE_DEPTH

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_signal;           // binary: drain task wake
static TaskHandle_t      s_drain_task;
static bool              s_initialized;

// Atomic stats — readable without lock from heartbeat path.
static atomic_uint       s_total_enqueued;
static atomic_uint       s_total_transmitted;
static atomic_uint       s_total_dropped_overflow;
static atomic_uint       s_total_dropped_tx_err;
static atomic_uint       s_depth_cache;     // for nanojs8_tx_queue_count

// ── Small helpers ────────────────────────────────────────────────────

static void safe_strncpy(char *dst, size_t dst_n, const char *src)
{
    if (dst_n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = dst_n - 1;
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// Update the atomic depth cache. Caller holds the mutex.
static void update_depth_cache_locked(void)
{
    atomic_store(&s_depth_cache, (unsigned)s_count);
}

// ── Drain task ───────────────────────────────────────────────────────

static void drain_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "drain task running (CPU %d, depth=%d, wire_max=%d)",
             xPortGetCoreID(),
             NANOJS8_TX_QUEUE_DEPTH,
             NANOJS8_TX_QUEUE_WIRE_MAX);

    while (true) {
        // Block waiting for either a new enqueue signal or a 500 ms
        // timeout. The timeout means we'll recheck even if we missed
        // a signal (defense in depth — binary semaphores are
        // reliable, but a 500 ms wake cost is cheap insurance).
        xSemaphoreTake(s_signal, pdMS_TO_TICKS(500));

        // Wait for the audio TX path to become idle. tx_audio's
        // worker holds is_active across its full PTT cycle (~13 s
        // per frame). We poll every 500 ms — same cadence as PTT's
        // own watchdog so we don't burn cycles.
        while (nanojs8_tx_audio_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Take a copy of the head entry under the mutex, then release
        // the mutex before the transmit_text call (which can take
        // hundreds of milliseconds for the pack-cache rebuild).
        char wire[NANOJS8_TX_QUEUE_WIRE_MAX];
        char reason[NANOJS8_TX_QUEUE_REASON_MAX];
        bool have_entry = false;

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_count > 0) {
                memcpy(wire,   s_entries[s_head].wire,   sizeof(wire));
                memcpy(reason, s_entries[s_head].reason, sizeof(reason));
                s_head = (s_head + 1u) % NANOJS8_TX_QUEUE_DEPTH;
                --s_count;
                update_depth_cache_locked();
                have_entry = true;
            }
            xSemaphoreGive(s_mutex);
        } else {
            ESP_LOGW(TAG, "drain: mutex acquire timed out — re-trying");
            continue;
        }

        if (!have_entry) continue;

        ESP_LOGI(TAG, "drain: tx '%s' (reason=%s, depth-after-pop=%u)",
                 wire, reason, (unsigned)atomic_load(&s_depth_cache));

        // Hand off to tx_audio. transmit_text spawns its own worker
        // task (16 KB stack) for the encode/LDPC/modulate chain, so
        // this call is just a marshal — returns quickly. The actual
        // TX completes asynchronously; we'll see is_active() go
        // false again before processing the next queued entry.
        esp_err_t err = nanojs8_tx_audio_transmit_text(wire);
        if (err == ESP_OK) {
            atomic_fetch_add(&s_total_transmitted, 1);
        } else {
            atomic_fetch_add(&s_total_dropped_tx_err, 1);
            ESP_LOGE(TAG, "drain: transmit_text failed for '%s' "
                          "(reason=%s, err=%s)",
                     wire, reason, esp_err_to_name(err));
            // Don't retry on tx error — typically caused by a state
            // mismatch (audio not ready, PTT inhibited, etc.) that
            // won't resolve quickly. Drop the entry and move on.
        }
    }
}

// ── API ──────────────────────────────────────────────────────────────

esp_err_t nanojs8_tx_queue_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "init: failed to create mutex");
        return ESP_FAIL;
    }
    s_signal = xSemaphoreCreateBinary();
    if (!s_signal) {
        ESP_LOGE(TAG, "init: failed to create signal semaphore");
        return ESP_FAIL;
    }

    // L7.16-fix2: drain task stack is now 16 KB and lives in PSRAM
    // (was 4 KB DRAM). REASON: L7.14-fix9 added a synchronous
    // nanojs8_js8_text_frame_count() pre-flight inside transmit_text()
    // which runs std::regex via Varicode::pack on the caller's stack.
    // Per the project journal that chain needs 14-20 KB. The first
    // auto-ACK from js8sync (KD8PGB/P MSG round-trip) crashed
    // tx_queue_drain with a stack overflow because the wire was
    // fresh (pack-cache cold). The user-compose path doesn't trip
    // it because screen_compose calls text_frame_count itself from
    // the UI task first, leaving the pack-cache hot when
    // tx_queue_drain later re-runs the check.
    //
    // PSRAM stack is safe HERE because the drain → transmit_text path
    // is pure DSP/encode work; no NVS writes. If anything reachable
    // from this task ever needs to write NVS (mailbox, config), it
    // MUST route through the mailbox's persist worker (DRAM stack)
    // or the equivalent — see L7.16 for the precedent. Direct NVS
    // writes from this PSRAM-stacked task would crash on the SPI
    // cache-disable assertion. Comment is the gate; reviewers should
    // bounce any change that adds NVS calls reachable from the drain.
    //
    // CPU 0 (UI/USB core) — CPU 1 is saturated by js8_sync + tx_audio.
    // Priority 3: above idle, below tx_audio worker.
    const size_t kStackBytes = 16384;
    const BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        drain_task, "tx_queue_drain",
        /*stack=*/ kStackBytes,
        /*arg=*/ NULL,
        /*priority=*/ 3,
        &s_drain_task,
        /*core=*/ 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "init: xTaskCreatePinnedToCoreWithCaps failed "
                      "(rc=%d, likely PSRAM OOM)", (int)rc);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TX queue ready (depth=%d, wire_max=%d B, "
                  "footprint=%u B BSS)",
             NANOJS8_TX_QUEUE_DEPTH,
             NANOJS8_TX_QUEUE_WIRE_MAX,
             (unsigned)sizeof(s_entries));
    return ESP_OK;
}

esp_err_t nanojs8_tx_queue_enqueue(const char *wire, const char *reason)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "enqueue: not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wire || wire[0] == '\0') {
        ESP_LOGW(TAG, "enqueue: empty wire — refusing");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "enqueue: mutex timeout");
        return ESP_FAIL;
    }

    bool evicted = false;
    if (s_count >= NANOJS8_TX_QUEUE_DEPTH) {
        // Drop oldest (head) to make room. ACK obligations are
        // time-sensitive; the most recent ones win.
        ESP_LOGW(TAG, "enqueue: full — dropping oldest '%s' "
                      "(reason=%s, queued %lld ms ago)",
                 s_entries[s_head].wire,
                 s_entries[s_head].reason,
                 (long long)((esp_timer_get_time() -
                              s_entries[s_head].at_us) / 1000));
        s_head = (s_head + 1u) % NANOJS8_TX_QUEUE_DEPTH;
        --s_count;
        atomic_fetch_add(&s_total_dropped_overflow, 1);
        evicted = true;
    }

    // Insert at tail.
    tx_queue_entry_t *slot = &s_entries[s_tail];
    memset(slot, 0, sizeof(*slot));
    safe_strncpy(slot->wire,   sizeof(slot->wire),   wire);
    safe_strncpy(slot->reason, sizeof(slot->reason), reason);
    slot->at_us = esp_timer_get_time();

    s_tail = (s_tail + 1u) % NANOJS8_TX_QUEUE_DEPTH;
    ++s_count;
    update_depth_cache_locked();
    atomic_fetch_add(&s_total_enqueued, 1);

    ESP_LOGI(TAG, "enqueue: '%s' (reason=%s, depth=%u%s)",
             wire,
             reason ? reason : "",
             (unsigned)s_count,
             evicted ? " — evicted oldest" : "");

    xSemaphoreGive(s_mutex);

    // Wake the drain task. xSemaphoreGive on a binary semaphore is
    // a no-op if it's already "given" — that's intentional, the
    // drain task only needs to see "at least one item present" not
    // "exactly N items present".
    xSemaphoreGive(s_signal);

    return ESP_OK;
}

uint32_t nanojs8_tx_queue_count(void)
{
    if (!s_initialized) return 0;
    return atomic_load(&s_depth_cache);
}

void nanojs8_tx_queue_get_stats(nanojs8_tx_queue_stats_t *out)
{
    if (!out) return;
    out->depth                  = nanojs8_tx_queue_count();
    out->total_enqueued         = atomic_load(&s_total_enqueued);
    out->total_transmitted      = atomic_load(&s_total_transmitted);
    out->total_dropped_overflow = atomic_load(&s_total_dropped_overflow);
    out->total_dropped_tx_err   = atomic_load(&s_total_dropped_tx_err);
}
