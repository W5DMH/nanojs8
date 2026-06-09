/*
 * js8_sync.h — JS8 Normal-mode sync detection (L7.5)
 * =====================================================================
 * Consumes slot-aligned 12 kHz int16 audio from nanojs8_rx_audio, feeds
 * it through ft8_lib's monitor (with Costas pattern swapped for JS8 in
 * components/nanojs8_ft8_lib/ft8/constants.c), and reports sync
 * candidates per slot.
 *
 * At this layer there is NO LDPC decode and NO message extraction. The
 * output is only candidate locations: (frequency, time_offset, score).
 * A candidate is a position in the waterfall where the JS8 Costas array
 * appears with high correlation. Subsequent layers will:
 *
 *   L7.6 — feed each candidate through bpdecode174 (lifted from gfsk8)
 *          to recover the 87-bit message frame
 *   L7.7 — unpack frames via Varicode + JSC into human-readable text
 *
 * Heartbeat-visible metrics:
 *   slots_processed     total slots consumed
 *   last_slot_candidates count from the most recent slot
 *   total_candidates    cumulative
 *   last_slot_best_score peak Costas-score seen in last slot
 *   last_slot_cpu_ms    wall-clock cost of one slot's sync analysis
 *   stack_min_free      task stack high-water-mark (B free)
 *
 * Memory footprint (one-time at start):
 *   - Monitor working buffers (DRAM static): ~48 KB
 *   - FFT plan + work area (DRAM static):    ~32 KB
 *   - Waterfall (PSRAM heap):                ~333 KB
 *   - Slot snapshot buffer (PSRAM heap):     ~360 KB
 *   - Decoder task stack:                    ~20 KB
 *
 * Threading: ONE FreeRTOS task on Core 1 (priority 5). All state is
 * task-private except the atomic stats struct exposed via get_stats().
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t slots_processed;
    uint32_t last_slot_candidates;
    uint32_t total_candidates;
    int16_t  last_slot_best_score;   // Costas correlation, higher = better
    uint32_t last_slot_cpu_ms;
    uint32_t stack_min_free;
    // L7.6: LDPC + CRC decode results
    uint32_t last_slot_attempts;     // candidates we tried to decode in last slot
    uint32_t last_slot_decodes;      // CRC-passing decodes in last slot
    uint32_t total_decodes;          // cumulative CRC-passing decodes
} nanojs8_js8_sync_stats_t;

/**
 * Start the JS8 sync task. Spawns a single Core-1 task that:
 *   1. Allocates a slot snapshot buffer (360 KB PSRAM)
 *   2. Initializes ft8_lib's monitor (12 kHz, 200-3000 Hz, freq_osr=2)
 *   3. Waits for UTC to be set via SETUP
 *   4. Polls rx_audio's slots_fired counter every 200 ms; on slot
 *      completion, snapshots audio, feeds the monitor in block-sized
 *      chunks, then calls ftx_find_candidates to extract syncs.
 *
 * Returns:
 *   ESP_OK                 task created and resources allocated
 *   ESP_ERR_NO_MEM         OOM somewhere in the chain (logged separately)
 *   ESP_ERR_INVALID_STATE  rx_audio or time not yet started
 */
esp_err_t nanojs8_js8_sync_start(void);

/**
 * Sample current sync-detector stats. Safe from any task. Reads are
 * lock-free via atomics; per-field tearing is acceptable since the
 * fields are diagnostic only.
 */
void nanojs8_js8_sync_get_stats(nanojs8_js8_sync_stats_t *out);

#ifdef __cplusplus
}
#endif
