// NanoJS8 — Radio RX ring buffer
//
// A single-producer, single-consumer audio sample buffer that decouples
// the USB UAC interrupt callback (writer) from the Phase 4 decode task
// (reader, future).
//
// Design notes:
//
//   - Backed by a FreeRTOS StreamBuffer. StreamBuffer is purpose-built
//     for SPSC byte streams between an ISR-context producer and a task-
//     context consumer. We treat each int16_t sample as 2 bytes; the
//     wrapper handles the conversion.
//
//   - Capacity is fixed at compile time. Phase 3a uses 16 KB = 8192
//     samples = ~170 ms at 48 kHz mono. Plenty for any Phase 4 decode
//     pipeline (FT8 uses 1.92 s windows; that buffer lives in the
//     decoder's own ring, not here).
//
//   - Drop policy: if the ring is full, oldest samples are dropped
//     silently, and an overrun counter increments. Logged at WARN once
//     per 100 overruns to avoid log spam.
//
//   - Phase 3a doesn't actually consume from the ring (no Phase 4 yet).
//     The buffer fills and overruns continuously when audio is flowing.
//     The serial-monitor `radio status` command shows the overrun
//     counter, which is the smoke test for "audio is flowing."
//
// Threading model:
//   write(): called from the UAC frame callback (task context — the UAC
//            component runs its own task, not ISR). Single producer.
//   read():  called from Phase 4 decoder task. Single consumer.
//   drain(): called from radio_service (e.g. on disconnect) to discard
//            buffered audio. The implementation makes this safe to call
//            even if a read() is in progress in another task.
//   overrun_count(): readable from any task; uses atomic relaxed load.

#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

namespace nanojs8 {
namespace radio {

// Capacity in bytes. 16 KB chosen as 8192 mono 16-bit samples = ~170 ms
// at 48 kHz. Adjusting up costs DRAM proportionally; adjusting down
// risks dropouts if the UAC callback is delayed by another high-prio
// task. 16 KB is a comfortable middle for Phase 3a.
constexpr size_t RX_BUFFER_CAPACITY_BYTES = 16 * 1024;

// Initialize the singleton ring buffer. Idempotent; safe to call
// multiple times — only the first call allocates.
bool rx_buffer_init(void);

// Tear down the ring buffer. Frees its backing storage. Used on
// radio_service::stop() and during Phase 7 EXIT.
void rx_buffer_destroy(void);

// Write samples produced by UAC. Returns the number of samples actually
// stored. When the buffer is nearly full, this drops the OLDEST samples
// (the just-arrived audio is more useful than stale audio that a slow
// consumer never got to) and increments overrun_count.
//
// Called from the UAC frame callback in task context. Block time 0;
// never waits.
size_t rx_buffer_write(const int16_t* samples, size_t count);

// Read up to max_count samples into dest. Returns the number actually
// read. Block time 0; returns 0 immediately if no samples available.
//
// Called from the Phase 4 decode task. In Phase 3a, called only from
// serial-monitor diagnostic commands.
size_t rx_buffer_read(int16_t* dest, size_t max_count);

// Number of samples currently buffered. Approximate (may be off by a
// few samples in the presence of concurrent writes).
size_t rx_buffer_available(void);

// Discard all buffered samples. Safe to call from any task.
void rx_buffer_drain(void);

// Monotonic overrun counter — incremented each time write() had to drop
// samples because the buffer was full. Not reset by drain(); reset only
// by rx_buffer_destroy() + rx_buffer_init().
uint32_t rx_buffer_overrun_count(void);

// Monotonic total-frames counter — incremented per write() call (one
// frame = one UAC callback's worth of samples, typically 48 samples =
// 1 ms at 48 kHz mono). Used by the serial-monitor diagnostic command
// to show "audio is flowing" without needing to actually consume.
uint32_t rx_buffer_frames_total(void);

} // namespace radio
} // namespace nanojs8
