/*
 * rx_audio.cpp — Decimator + ring + slot trigger (L7.1)
 * ======================================================
 * See rx_audio.h for the architecture overview. This file implements
 * the two background tasks and their state.
 *
 * Memory ownership
 * ================
 *   s_ring         — 1.44 MB int16_t ring buffer in PSRAM, malloc'd
 *                    once at start; never freed (lifetime = program).
 *
 * Concurrency invariants
 * ======================
 *   - Only `rx_audio_task` writes to s_ring + s_ring_write_pos
 *   - `slot_task` reads s_ring (via snapshot helper) without locking;
 *     since the ring is much larger than one slot and the writer
 *     advances at ~12k samples/s, the slot's worth of bytes the
 *     reader copies (180k bytes) won't be overwritten before memcpy
 *     completes (a few ms)
 *   - Stats are std::atomic<uint32_t> for lock-free heartbeat reads
 *
 * License: GPL-3.0
 */

#include "rx_audio.h"
#include "audio.h"
#include "time_source.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <atomic>
#include <string.h>

static const char *TAG = "rx_audio";

namespace {

// Decimation ratio: 48 kHz USB → 12 kHz internal.
constexpr uint32_t DECIMATION_RATIO = 4;

// USB-read chunk size at 48 kHz. 4 KB = 2048 int16_t samples ≈ 42 ms
// per chunk. Matches the existing loopback's chunk and the UAC driver's
// internal buffer threshold (defined in audio.cpp:72).
constexpr size_t  USB_READ_BYTES   = 4096;
constexpr size_t  USB_READ_SAMPLES = USB_READ_BYTES / sizeof(int16_t);

// FreeRTOS task stack sizes. 4 KB is comfortable for these tasks
// (no deep recursion, small locals).
constexpr uint32_t RX_AUDIO_TASK_STACK = 4096;
constexpr uint32_t SLOT_TASK_STACK     = 4096;

// Task priorities. Decimator runs higher than slot trigger because
// it has hard real-time deadlines (USB FIFO will overrun if we don't
// drain ~42 ms cadence). Slot trigger is soft real-time.
constexpr UBaseType_t RX_AUDIO_TASK_PRIO = 5;
constexpr UBaseType_t SLOT_TASK_PRIO     = 4;

// Ring buffer + write position. Allocated at start; never freed.
int16_t  *s_ring = nullptr;
std::atomic<uint32_t> s_ring_write_pos{0};    // 0 .. RING_SAMPLES-1
std::atomic<uint32_t> s_ring_samples_total{0};// monotonic total

// Decimator counters.
std::atomic<uint32_t> s_decimator_samples_in{0};

// Slot trigger counters.
std::atomic<uint32_t> s_slots_fired{0};
std::atomic<uint32_t> s_slots_skipped_no_utc{0};
std::atomic<uint32_t> s_slots_skipped_short{0};
std::atomic<int32_t>  s_last_slot_peak{0};        // signed for compatibility
std::atomic<uint32_t> s_last_slot_seconds_today{0};
std::atomic<uint8_t>  s_last_slot_had_utc{0};

// Started flag for idempotent start.
std::atomic<bool> s_started{false};

// ── Decimator + ring writer ────────────────────────────────────────

// Apply 4:1 stride decimation from src[0..src_samples-1] to the ring.
// Returns the number of decimated samples written.
//
// Note: src_samples may not be a multiple of 4; we just take every
// 4th starting at index 0. Whatever phase remainder is left after this
// chunk gets the same phase on the next chunk (no inter-chunk drift —
// USB chunks always arrive on their own boundary).
size_t decimate_and_write(const int16_t *src, size_t src_samples) {
    if (!s_ring) return 0;
    uint32_t pos = s_ring_write_pos.load(std::memory_order_relaxed);
    size_t written = 0;
    for (size_t i = 0; i < src_samples; i += DECIMATION_RATIO) {
        s_ring[pos] = src[i];
        pos = (pos + 1) % NANOJS8_RX_AUDIO_RING_SAMPLES;
        ++written;
    }
    s_ring_write_pos.store(pos, std::memory_order_release);
    s_ring_samples_total.fetch_add(written, std::memory_order_relaxed);
    return written;
}

void rx_audio_task(void *arg) {
    (void)arg;
    static uint8_t  usb_buf[USB_READ_BYTES];
    ESP_LOGI(TAG, "Decimator task entered (USB chunk %u bytes, ratio %u:1)",
             (unsigned)USB_READ_BYTES, (unsigned)DECIMATION_RATIO);

    while (true) {
        // Wait until the audio stream is ready.
        nanojs8_audio_stream_info_t rx_info;
        nanojs8_audio_rx_info(&rx_info);
        if (rx_info.status != NANOJS8_AUDIO_STATUS_READY) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Sample rate sanity check. The discovery layer is supposed
        // to negotiate 48 kHz; if we ever end up at a different rate
        // our decimation ratio is wrong. Log+skip rather than corrupt.
        if (rx_info.sample_rate != 48000) {
            ESP_LOGW(TAG, "Unexpected RX sample rate %u Hz (expected 48000) "
                          "— decimator skipping until rate corrects",
                     (unsigned)rx_info.sample_rate);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t got = 0;
        esp_err_t err = nanojs8_audio_read(usb_buf, sizeof(usb_buf),
                                            &got, 100);
        if (err != ESP_OK || got == 0) {
            // Read returned no data or transient error. The 100 ms
            // read timeout already paces us; just loop.
            continue;
        }

        // got is in BYTES. At 16-bit mono, each sample is 2 bytes.
        // (We assume mono because audio.h's NANOJS8_AUDIO_CHANNELS_RX
        // is 1; if a stereo device ever shows up, the discovery path
        // would log a mismatch.)
        size_t in_samples = got / sizeof(int16_t);
        s_decimator_samples_in.fetch_add((uint32_t)in_samples,
                                          std::memory_order_relaxed);
        decimate_and_write(reinterpret_cast<int16_t*>(usb_buf), in_samples);
    }
}

// ── Slot trigger ──────────────────────────────────────────────────

// Copy the last `slot_samples` from the ring into `out`. Returns false
// if the ring hasn't accumulated that much audio yet (early boot). This
// helper does the wrap-around math; callers don't need to.
bool copy_last_n(int16_t *out, size_t n) {
    uint32_t total = s_ring_samples_total.load(std::memory_order_acquire);
    if (total < n) return false;
    uint32_t pos = s_ring_write_pos.load(std::memory_order_acquire);
    // "Last n samples" means [pos-n .. pos-1] modulo ring size.
    uint32_t start = (pos + NANOJS8_RX_AUDIO_RING_SAMPLES - (uint32_t)n) %
                     NANOJS8_RX_AUDIO_RING_SAMPLES;
    if (start + n <= NANOJS8_RX_AUDIO_RING_SAMPLES) {
        // Contiguous slice — single memcpy.
        memcpy(out, &s_ring[start], n * sizeof(int16_t));
    } else {
        // Wraps around end of ring — two memcpys.
        size_t first = NANOJS8_RX_AUDIO_RING_SAMPLES - start;
        memcpy(out, &s_ring[start], first * sizeof(int16_t));
        memcpy(out + first, &s_ring[0], (n - first) * sizeof(int16_t));
    }
    return true;
}

void slot_task(void *arg) {
    (void)arg;
    // L7.1 sizing trap: NANOJS8_RX_AUDIO_SLOT_SAMPLES × sizeof(int16_t)
    // = 180_000 × 2 = 360 KB. That's larger than internal DRAM has
    // free (~264 KB at runtime). A `static int16_t slot_buf[N]` would
    // either fail to link or kill us during BSS zero-init. So we
    // allocate the snapshot buffer in PSRAM at task entry — keeps
    // DRAM untouched for the small allocations that NEED it (FreeRTOS
    // stacks, USB driver internals, etc.).
    int16_t *slot_buf = static_cast<int16_t*>(
        heap_caps_malloc(NANOJS8_RX_AUDIO_SLOT_SAMPLES * sizeof(int16_t),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slot_buf) {
        ESP_LOGE(TAG, "Slot task: failed to allocate %u-byte PSRAM snapshot "
                      "buffer (free=%u) — slot trigger disabled",
                 (unsigned)(NANOJS8_RX_AUDIO_SLOT_SAMPLES * sizeof(int16_t)),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelete(nullptr);
        return;  // unreachable, but keeps the static analyzer happy
    }

    ESP_LOGI(TAG, "Slot trigger task entered (JS8 Normal: 15000 ms, "
                  "snapshot %u KB in PSRAM)",
             (unsigned)(NANOJS8_RX_AUDIO_SLOT_SAMPLES * sizeof(int16_t) / 1024));

    bool announced_waiting = false;

    while (true) {
        if (!nanojs8_time_is_set()) {
            if (!announced_waiting) {
                ESP_LOGI(TAG, "Waiting for UTC to be set via SETUP before "
                              "slot trigger arms");
                announced_waiting = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (announced_waiting) {
            ESP_LOGI(TAG, "UTC available — slot trigger arming");
            announced_waiting = false;
        }

        // Compute ms until next 15 s slot boundary.
        uint32_t now_ms = nanojs8_time_millis_today();
        uint32_t into_slot = now_ms % NANOJS8_JS8_NORMAL_SLOT_MS;
        uint32_t to_next   = NANOJS8_JS8_NORMAL_SLOT_MS - into_slot;
        // If we're VERY close to the boundary (< 5 ms), advance to the
        // NEXT one to avoid firing the same boundary twice in a row.
        if (to_next < 5) {
            to_next += NANOJS8_JS8_NORMAL_SLOT_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(to_next));

        // We're at (or a few ms past) a slot boundary. Note: the
        // FreeRTOS tick is 10 ms by default, so our sleep precision
        // is ±5 ms — well within JS8's ±100 ms slot tolerance.

        uint32_t slot_seconds = nanojs8_time_seconds_today();
        s_last_slot_seconds_today.store(slot_seconds,
                                         std::memory_order_release);
        s_last_slot_had_utc.store(1, std::memory_order_release);

        // Snapshot the most recent slot's worth.
        if (!copy_last_n(slot_buf, NANOJS8_RX_AUDIO_SLOT_SAMPLES)) {
            s_slots_skipped_short.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGD(TAG, "Slot %u: ring not yet full enough; skipping",
                     (unsigned)slot_seconds);
            continue;
        }

        // Compute peak |sample| for the heartbeat. INT16_MIN can't be
        // safely negated (overflow), so clamp before abs.
        int32_t peak = 0;
        for (size_t i = 0; i < NANOJS8_RX_AUDIO_SLOT_SAMPLES; ++i) {
            int32_t s = slot_buf[i];
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        if (peak > INT16_MAX) peak = INT16_MAX;
        s_last_slot_peak.store(peak, std::memory_order_release);

        s_slots_fired.fetch_add(1, std::memory_order_relaxed);

        // L7.1: no consumer yet. L7.2 will call into the sync detector
        // here with slot_buf + slot_seconds. For L7.1 we just log peak.
        ESP_LOGD(TAG, "Slot fired: UTC second-of-day=%u peak=%d samples=%u",
                 (unsigned)slot_seconds, (int)peak,
                 (unsigned)NANOJS8_RX_AUDIO_SLOT_SAMPLES);
    }
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────

extern "C" esp_err_t nanojs8_rx_audio_start(void) {
    if (s_started.load(std::memory_order_acquire)) {
        return ESP_OK;
    }

    // Allocate the ring buffer in PSRAM. SPIRAM_8BIT_ACCESSIBLE is the
    // standard cap for PSRAM-backed allocations.
    size_t ring_bytes = NANOJS8_RX_AUDIO_RING_SAMPLES * sizeof(int16_t);
    s_ring = static_cast<int16_t*>(
        heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_ring) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for ring buffer "
                      "(free=%u)",
                 (unsigned)ring_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    memset(s_ring, 0, ring_bytes);
    ESP_LOGI(TAG, "Ring buffer allocated: %u samples @ %u Hz (%u KB PSRAM)",
             (unsigned)NANOJS8_RX_AUDIO_RING_SAMPLES,
             (unsigned)NANOJS8_RX_AUDIO_SAMPLE_RATE,
             (unsigned)(ring_bytes / 1024));

    // Spawn the two tasks pinned to Core 1 (Core 0 hosts the USB host
    // task, lwIP, etc. — keep audio off it).
    BaseType_t r = xTaskCreatePinnedToCore(
        rx_audio_task, "rx_audio", RX_AUDIO_TASK_STACK,
        nullptr, RX_AUDIO_TASK_PRIO, nullptr, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn decimator task");
        heap_caps_free(s_ring);
        s_ring = nullptr;
        return ESP_ERR_NO_MEM;
    }
    r = xTaskCreatePinnedToCore(
        slot_task, "rx_slot", SLOT_TASK_STACK,
        nullptr, SLOT_TASK_PRIO, nullptr, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn slot trigger task");
        // Decimator is already running; we can't easily kill it
        // here without more state. Leave it running — it's harmless
        // (just decimating to a ring nobody reads from).
        return ESP_ERR_NO_MEM;
    }

    s_started.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "RX audio pipeline started (decimator + slot trigger on Core 1)");
    return ESP_OK;
}

extern "C" void nanojs8_rx_audio_get_stats(nanojs8_rx_audio_stats_t *out) {
    if (!out) return;
    out->decimator_samples_in  =
        s_decimator_samples_in.load(std::memory_order_acquire);
    out->ring_samples_total    =
        s_ring_samples_total.load(std::memory_order_acquire);
    out->ring_write_pos        =
        s_ring_write_pos.load(std::memory_order_acquire);
    out->slots_fired           =
        s_slots_fired.load(std::memory_order_acquire);
    out->slots_skipped_no_utc  =
        s_slots_skipped_no_utc.load(std::memory_order_acquire);
    out->slots_skipped_short   =
        s_slots_skipped_short.load(std::memory_order_acquire);
    out->last_slot_peak        =
        (int16_t)s_last_slot_peak.load(std::memory_order_acquire);
    out->last_slot_seconds_today =
        s_last_slot_seconds_today.load(std::memory_order_acquire);
    out->last_slot_had_utc     =
        s_last_slot_had_utc.load(std::memory_order_acquire) != 0;
}

extern "C" bool nanojs8_rx_audio_snapshot_last_slot(int16_t *out,
                                                    size_t out_samples) {
    if (!out || out_samples != NANOJS8_RX_AUDIO_SLOT_SAMPLES) return false;
    if (!nanojs8_time_is_set()) return false;
    return copy_last_n(out, out_samples);
}
