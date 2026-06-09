/*
 * rx_audio.h — RX audio decimation + ring buffer + slot trigger (L7.1)
 * =====================================================================
 * Sits between the USB audio RX stream and future JS8 layers:
 *
 *   USB audio (48 kHz mono)
 *     │
 *     │  nanojs8_audio_read()
 *     ▼
 *   This component:
 *     - Decimate 4:1 → 12 kHz mono int16_t (matches MicroJS8)
 *     - Write to 60-second ring buffer in PSRAM (1.44 MB)
 *     - Slot trigger: fires when (UTC millis_today % 15000 == 0)
 *     │
 *     │  nanojs8_rx_audio_snapshot_last_slot()
 *     ▼
 *   L7.2 sync detector (future)
 *   L7.3 decoder (future)
 *
 * Decimation strategy
 * ===================
 * Simple stride decimation: every 4th sample. No anti-alias filter.
 *
 * Rationale (matches MicroJS8):
 *   - JS8 audio occupies ~500-2500 Hz, well below new Nyquist of 6 kHz
 *   - SSB receivers low-pass-filter to ~3 kHz before our USB capture
 *   - Adding a FIR anti-alias filter costs CPU we don't need to spend
 *
 * Worst case: high-frequency noise above 6 kHz folds into 0-6 kHz band.
 * Impact is a marginal SNR degradation, well within JS8's noise margin.
 *
 * Slot trigger
 * ============
 * The trigger task computes "ms until next 15 s boundary" from
 * nanojs8_time_millis_today() and sleeps until just past. On wake,
 * it snapshots the most recent 15 s of ring buffer and (future)
 * hands it to the sync detector.
 *
 * Trigger is GATED on UTC being set — without time sync, slot
 * boundaries don't align with real-world JS8 traffic. While waiting
 * for UTC the task sleeps in 1-second polls (cheap).
 *
 * Once L7.2 lands, the snapshot path will become the entry point for
 * the decode pipeline. For L7.1 the snapshot is just logged.
 *
 * Threading
 * =========
 *   - rx_audio_task        (core 1, prio 5) — reads USB audio, decimates,
 *                                              writes to ring
 *   - rx_audio_slot_task   (core 1, prio 4) — slot-boundary timing,
 *                                              snapshots, future decode call
 *
 * Stats are atomics — heartbeat reads without locks.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// JS8 Normal mode constants. L7.1 only supports Normal; other speeds
// (Fast 10 s, Turbo 6 s, Slow 30 s) are deferred and would just become
// additional NANOJS8_JS8_SLOT_* constants when added.
#define NANOJS8_RX_AUDIO_SAMPLE_RATE     12000U
#define NANOJS8_JS8_NORMAL_SLOT_MS       15000U
// L7.14-fix11 Path A: 60 s → 30 s. Slot trigger only ever needs the
// most recent 15 s slot of audio for the JS8 sync detector; the rest
// of the ring is processing margin. Boot logs show slot CPU is 2-4 s
// per slot, so 30 s (= 15 s margin beyond the last slot) is still
// very comfortable. Frees 720 KB of PSRAM at boot which Path B's
// multi-frame TX refactor needs to make the 8-frame ceiling reachable.
#define NANOJS8_RX_AUDIO_RING_SECONDS    30U
#define NANOJS8_RX_AUDIO_RING_SAMPLES    (NANOJS8_RX_AUDIO_SAMPLE_RATE * \
                                          NANOJS8_RX_AUDIO_RING_SECONDS)

// Snapshot size for one JS8 Normal slot of decimated audio.
#define NANOJS8_RX_AUDIO_SLOT_SAMPLES    (NANOJS8_RX_AUDIO_SAMPLE_RATE * \
                                          NANOJS8_JS8_NORMAL_SLOT_MS / 1000U)

// Start the RX audio pipeline. Idempotent. Spawns the decimator
// task and the slot-trigger task. Returns ESP_OK on success.
//
// Requires nanojs8_audio_start() to have been called (we read from
// its RX FIFO via nanojs8_audio_read).
//
// Allocates ~1.44 MB in PSRAM for the ring buffer; will return
// ESP_ERR_NO_MEM if that allocation fails (would indicate the PSRAM
// pool is fragmented or another component grabbed too much).
esp_err_t nanojs8_rx_audio_start(void);

// Heartbeat / diagnostics counters.
typedef struct {
    uint32_t decimator_samples_in;   // 48k input samples processed
    uint32_t ring_samples_total;     // 12k samples ever written to ring
    uint32_t ring_write_pos;         // current write index (0..RING_SAMPLES-1)
    uint32_t slots_fired;            // slot-boundary triggers since boot
    uint32_t slots_skipped_no_utc;   // slots we missed waiting for UTC
    uint32_t slots_skipped_short;    // slots fired before ring had 15 s
    int16_t  last_slot_peak;         // |max sample| from last slot snapshot
    uint32_t last_slot_seconds_today;// UTC seconds at last slot trigger
    bool     last_slot_had_utc;      // false until first time-set + first slot
} nanojs8_rx_audio_stats_t;

// Read current stats into the caller's buffer. Safe to call from any task;
// stats are read with atomic loads so no locking is required.
void nanojs8_rx_audio_get_stats(nanojs8_rx_audio_stats_t *out);

// Copy the most recent slot's worth of decimated audio into the caller's
// buffer. `out_samples` MUST be NANOJS8_RX_AUDIO_SLOT_SAMPLES. Returns
// true if the snapshot was successfully filled (UTC was set and the ring
// has at least one slot's worth of audio), false otherwise (output is
// untouched in that case).
//
// Intended caller: L7.2 sync detector, on the slot-trigger callback.
// For L7.1 there's no caller yet — but exposing this now means L7.2 is
// a drop-in addition rather than an API renegotiation.
bool nanojs8_rx_audio_snapshot_last_slot(int16_t *out, size_t out_samples);

#ifdef __cplusplus
}
#endif
