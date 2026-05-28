// NanoJS8 — Radio RX ring buffer implementation.
//
// Wraps FreeRTOS StreamBuffer for SPSC audio sample passing from the
// UAC frame callback (producer, task context) to the Phase 4 decode
// task (consumer, future).
//
// FreeRTOS StreamBuffer chosen because:
//   - Native SPSC, no mutex overhead
//   - Built-in length-prefixed FIFO semantics that match audio framing
//   - Returns immediately when full (configurable; we want non-blocking)
//   - Documented thread-safety guarantees
//
// Overrun semantics: when the buffer fills, instead of dropping the
// just-arrived audio (which is what we'd hear next), we drain the
// oldest samples to make room. This costs slightly more CPU per
// overrun event but keeps the buffer's contents "the most recent
// samples we have," which is what a real-time audio pipeline wants.

#include "radio_rx_buffer.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

namespace nanojs8 {
namespace radio {

static const char* TAG = "radio_rx";

// Singleton state.
static StreamBufferHandle_t      s_stream         = nullptr;
static std::atomic<uint32_t>     s_overrun_count{0};
static std::atomic<uint32_t>     s_frames_total{0};
static std::atomic<uint32_t>     s_overrun_log_counter{0};

bool rx_buffer_init(void) {
    if (s_stream) {
        return true;  // idempotent
    }

    // Trigger level = 1: as soon as ANY bytes are available, a waiting
    // reader unblocks. We don't actually use blocking reads in Phase 3a
    // (consumer hasn't been written yet), but this is the right default
    // for the Phase 4 decode loop's eventual blocking-read pattern.
    s_stream = xStreamBufferCreate(RX_BUFFER_CAPACITY_BYTES, /*trigger=*/1);
    if (!s_stream) {
        ESP_LOGE(TAG, "xStreamBufferCreate(%u) failed",
                 (unsigned)RX_BUFFER_CAPACITY_BYTES);
        return false;
    }

    s_overrun_count.store(0, std::memory_order_relaxed);
    s_frames_total.store(0, std::memory_order_relaxed);
    s_overrun_log_counter.store(0, std::memory_order_relaxed);

    ESP_LOGI(TAG, "RX buffer initialized: capacity=%u bytes (~%u ms @ 48kHz mono)",
             (unsigned)RX_BUFFER_CAPACITY_BYTES,
             (unsigned)(RX_BUFFER_CAPACITY_BYTES / 2 / 48));
    return true;
}

void rx_buffer_destroy(void) {
    if (s_stream) {
        vStreamBufferDelete(s_stream);
        s_stream = nullptr;
    }
    // Counters reset only on destroy so frames_total survives drain().
    s_overrun_count.store(0, std::memory_order_relaxed);
    s_frames_total.store(0, std::memory_order_relaxed);
    s_overrun_log_counter.store(0, std::memory_order_relaxed);
}

size_t rx_buffer_write(const int16_t* samples, size_t count) {
    if (!s_stream || !samples || count == 0) {
        return 0;
    }

    s_frames_total.fetch_add(1, std::memory_order_relaxed);

    const size_t bytes_in = count * sizeof(int16_t);

    // Check available space; if insufficient, drain oldest samples so
    // we can write the new ones. This is the "discard oldest" overrun
    // policy. xStreamBufferSpacesAvailable is a cheap lookup.
    const size_t spaces_avail = xStreamBufferSpacesAvailable(s_stream);
    if (spaces_avail < bytes_in) {
        const size_t to_drop = bytes_in - spaces_avail;
        // Discard to_drop bytes from the head. Use a small stack buffer
        // and a loop in case to_drop > one read can grab. 256 is a
        // reasonable trade-off between stack use and loop iterations
        // (UAC callbacks typically deliver 192 bytes = ~1 ms of audio
        // mono, so one or two iterations is the usual case).
        uint8_t discard[256];
        size_t remaining = to_drop;
        while (remaining > 0) {
            const size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
            const size_t got = xStreamBufferReceive(s_stream, discard, chunk, 0);
            if (got == 0) {
                break;  // shouldn't happen, but bail rather than spin
            }
            remaining -= got;
        }

        // Throttled logging: log first overrun and then every 100th.
        const uint32_t prev = s_overrun_count.fetch_add(1, std::memory_order_relaxed);
        if (prev == 0 || (prev % 100) == 0) {
            ESP_LOGW(TAG, "RX overrun #%u — dropped %u bytes (no consumer?)",
                     (unsigned)(prev + 1), (unsigned)to_drop);
        }
    }

    const size_t bytes_written = xStreamBufferSend(
        s_stream, samples, bytes_in, 0);
    return bytes_written / sizeof(int16_t);
}

size_t rx_buffer_read(int16_t* dest, size_t max_count) {
    if (!s_stream || !dest || max_count == 0) {
        return 0;
    }
    const size_t bytes_max = max_count * sizeof(int16_t);
    const size_t bytes_read = xStreamBufferReceive(
        s_stream, dest, bytes_max, 0);  // non-blocking
    return bytes_read / sizeof(int16_t);
}

size_t rx_buffer_available(void) {
    if (!s_stream) return 0;
    return xStreamBufferBytesAvailable(s_stream) / sizeof(int16_t);
}

void rx_buffer_drain(void) {
    if (!s_stream) return;
    xStreamBufferReset(s_stream);
}

uint32_t rx_buffer_overrun_count(void) {
    return s_overrun_count.load(std::memory_order_relaxed);
}

uint32_t rx_buffer_frames_total(void) {
    return s_frames_total.load(std::memory_order_relaxed);
}

} // namespace radio
} // namespace nanojs8
