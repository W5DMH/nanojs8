/*
 * tx_audio.c — L7.11c-L7.11e TX audio path implementation
 * ========================================================
 *
 * See tx_audio.h for the public API contract.
 *
 * Threading:
 *   - init / render / self-test are called from the boot path (main task
 *     or the encoder self-test task — both single-threaded contexts).
 *   - transmit_text() spawns a dedicated worker task with 16 KB stack
 *     (sized for the modulate step, which uses std::regex inside
 *     gfsk8::pack). The worker modulates → renders → waits for the
 *     next JS8 slot boundary → keys PTT → streams chunks via
 *     nanojs8_audio_write() → releases PTT → self-deletes.
 *     UI / main loop are unaffected throughout.
 *   - is_active() is atomic-safe (a single volatile bool flag).
 *
 * License: GPL-3.0
 */

#include "tx_audio.h"

#include "js8_codec.h"
#include "audio.h"
#include "ptt.h"          // L7.11d: nanojs8_ptt_set
#include "radio.h"        // L7.11d: nanojs8_radio_get_active + profile
#include "time_source.h"  // L7.11d: nanojs8_time_millis_today + is_set
#include "config.h"       // L7.11e: read callsign + grid for modulate

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"  // L7.11h.2-fix2: xTaskCreatePinnedToCoreWithCaps

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tx_audio";

// ── State ────────────────────────────────────────────────────────────
//
// Single-instance: one buffer, one TX in flight at a time. Enforced
// via s_active. atomic_bool gives us race-free start/stop coordination
// between the UI thread (which calls transmit()) and the worker
// thread (which clears s_active on exit).

static int16_t      *s_stereo_buf       = NULL;   // PSRAM, 2.41 MB
static bool          s_init_ok          = false;
static bool          s_render_valid     = false;
static atomic_bool   s_active           = ATOMIC_VAR_INIT(false);
static TaskHandle_t  s_tx_task_handle   = NULL;

// ── Tunables ─────────────────────────────────────────────────────────
//
// USB UAC TX chunk size: stereo frames per nanojs8_audio_write call.
// 256 frames = 256 × 2 ch × 2 bytes = 1024 bytes per write = 5.33 ms
// at 48 kHz. Small enough to keep the UAC isochronous queue topped up
// without bursts; large enough to keep call-overhead negligible.
#define TX_CHUNK_FRAMES        256
#define TX_CHUNK_SAMPLES       (TX_CHUNK_FRAMES * NANOJS8_TX_AUDIO_CHANNELS)
#define TX_CHUNK_BYTES         (TX_CHUNK_SAMPLES * (int)sizeof(int16_t))

// Per-write timeout. If the UAC ring backs up beyond this we log and
// keep going (skip-ahead). 100 ms ≈ ~20× a single chunk, generous.
#define TX_WRITE_TIMEOUT_MS    100

// Worker task properties.
// L7.11e: stack bumped from 4 KB → 16 KB because the worker now runs
// nanojs8_js8_modulate_text() inline, which calls gfsk8::pack — and
// pack uses std::regex inside Varicode::buildMessageFrames (same
// stack-hungry chain that needed 32 KB for the boot self-test). The
// L7.11a measurement showed the encode chain uses ~3.8 KB; we size
// to 16 KB for the same 4× safety margin we use elsewhere.
#define TX_TASK_STACK          16384
#define TX_TASK_PRIORITY       5
#define TX_TASK_CORE           1     // Off CPU0 (UI lives there)
#define TX_TASK_NAME           "tx_test"

// ── L7.11f-fix2: continuous-PTT multi-frame burst tunables ───────────
//
// Cap on the number of frames we'll pre-modulate up front. Each cache
// buffer is NANOJS8_JS8_MODULATE_BUFFER_BYTES (~308 KB) of PSRAM. The
// log shows ~3.1 MB free PSRAM after all subsystems init. 8 frames ×
// 308 KB = 2.46 MB — comfortably within budget with margin for heap
// fragmentation. Beyond 8 frames the message is also taking 8 × 15 =
// 120 seconds of on-air time, well past the point where the operator
// should be using a different mode (or the LongMSG variant in a future
// layer). If pack() returns more than this we refuse the TX with a
// clear error rather than allocate beyond budget.
#define TX_MAX_FRAMES          8

// PTT watchdog grace beyond N × NANOJS8_PTT_WATCHDOG_MS for a burst.
// Mirrors MicroJS8's _CAT_WATCHDOG_GRACE_S = 5.0 s. Covers the worker's
// final tail-delay + ptt_off + the watchdog poll interval.
#define TX_BURST_WDT_GRACE_MS  5000

// Inter-frame silence bytes for a continuous-PTT burst. Each JS8 Normal
// slot is 15 000 ms; the rendered audio buffer is 13 140 ms (500 ms
// silence prefix + 12 640 ms tones). Gap = 1 860 ms. At 48 kHz stereo
// 16-bit that's 1.860 × 48000 × 2 ch × 2 B = 357 120 bytes. We round
// to the chunk size below so the loop is exact-fit (no partial chunk).
#define TX_INTERFRAME_SILENCE_MS 1860
// Bytes of silence per gap: 1.860 s × 48 000 Hz × 2 ch × 2 B/sample =
// 357 120 B (integer, exact). Then ceiling-divide by TX_CHUNK_BYTES to
// the next whole chunk: 357 120 / 1024 = 348.75 → 349 chunks →
// 357 376 B → 1 861.33 ms of actual silence. The +1.33 ms drift per
// frame is invisible against JS8's ±500 ms slot tolerance and
// accumulates to only +9 ms over 8 frames at the upper cap. Decoders
// never notice.
#define TX_INTERFRAME_SILENCE_BYTES \
        ((TX_INTERFRAME_SILENCE_MS * NANOJS8_TX_AUDIO_SAMPLE_RATE \
          * NANOJS8_TX_AUDIO_CHANNELS * (int)sizeof(int16_t)) / 1000)
#define TX_INTERFRAME_SILENCE_CHUNKS \
        ((TX_INTERFRAME_SILENCE_BYTES + TX_CHUNK_BYTES - 1) \
         / TX_CHUNK_BYTES)

// Zero-filled silence chunk used between frames during continuous-PTT
// bursts. BSS, zero-initialized once at load — never touched after.
// Lives in DRAM, not PSRAM, since we're streaming it constantly and
// PSRAM-read latency isn't free even though we have plenty of it.
static const int16_t s_silence_chunk[TX_CHUNK_SAMPLES] = {0};

// ── Init ─────────────────────────────────────────────────────────────

esp_err_t nanojs8_tx_audio_init(void)
{
    if (s_init_ok) {
        return ESP_OK;  // idempotent
    }

    s_stereo_buf = (int16_t *)heap_caps_malloc(
        NANOJS8_TX_AUDIO_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_stereo_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed: needed %d bytes "
                      "(free PSRAM = %u B)",
                 NANOJS8_TX_AUDIO_BUFFER_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    memset(s_stereo_buf, 0, NANOJS8_TX_AUDIO_BUFFER_BYTES);
    s_init_ok      = true;
    s_render_valid = false;

    ESP_LOGI(TAG, "Init OK: %d B PSRAM (%d stereo samples, "
                  "%.2f s of 48 kHz stereo audio)",
             NANOJS8_TX_AUDIO_BUFFER_BYTES,
             (int)NANOJS8_TX_AUDIO_STEREO_SAMPLES,
             (double)NANOJS8_TX_AUDIO_MONO_SAMPLES
               / (double)NANOJS8_TX_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

// ── Render: 12k mono → 48k stereo ────────────────────────────────────

esp_err_t nanojs8_tx_audio_render_from_modulator(void)
{
    if (!s_init_ok) {
        ESP_LOGE(TAG, "render: not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    size_t src_n = 0;
    const int16_t *src = nanojs8_js8_modulator_get_samples(&src_n);
    if (!src || src_n != NANOJS8_JS8_MODULATE_SAMPLE_COUNT) {
        ESP_LOGE(TAG, "render: modulator buffer unavailable "
                      "(src=%p n=%u expected=%d)",
                 (void *)src, (unsigned)src_n,
                 (int)NANOJS8_JS8_MODULATE_SAMPLE_COUNT);
        return ESP_FAIL;
    }

    // Linear 4× upsample, mono→stereo duplicate. Phase mapping:
    //   For each input pair (src[i], src[i+1]) we produce 4 mono outputs:
    //     out_mono[4i + k] = src[i] + (src[i+1] - src[i]) * k / 4
    //   k = 0,1,2,3 → samples at fractional positions 0.00, 0.25, 0.50, 0.75
    //   This is a length-4 zero-order-hold-equivalent in the average sense
    //   but with linear ramps between samples — much smoother spectrum.
    //
    //   For the final input sample (i = N-1) we have no src[i+1] to lean
    //   on. Holding (out = src[N-1]) for the trailing 4 samples is a clean
    //   tail; the modulator's last sample is part of a 1920-sample symbol
    //   so the signal is mid-cycle, not at a zero crossing — but the tail
    //   is silent anyway (it's after the data, no signal energy to worry
    //   about).
    //
    // Math is signed-int integer-only — int32 intermediate to dodge int16
    // overflow when interpolating (src[i+1]-src[i] can exceed 16-bit range
    // briefly during the multiply).

    const int64_t t_start_us = esp_timer_get_time();

    const size_t N = NANOJS8_JS8_MODULATE_SAMPLE_COUNT;
    int16_t *dst = s_stereo_buf;

    for (size_t i = 0; i < N - 1; ++i) {
        const int32_t s0 = (int32_t)src[i];
        const int32_t s1 = (int32_t)src[i + 1];
        const int32_t d  = s1 - s0;

        // k = 0 → exactly s0
        // k = 1..3 → linearly interpolate; (s1 - s0) * k / 4
        const int32_t m0 = s0;
        const int32_t m1 = s0 + (d * 1) / 4;
        const int32_t m2 = s0 + (d * 2) / 4;
        const int32_t m3 = s0 + (d * 3) / 4;

        // Duplicate each mono into L/R interleaved stereo.
        dst[0] = (int16_t)m0;  dst[1] = (int16_t)m0;
        dst[2] = (int16_t)m1;  dst[3] = (int16_t)m1;
        dst[4] = (int16_t)m2;  dst[5] = (int16_t)m2;
        dst[6] = (int16_t)m3;  dst[7] = (int16_t)m3;
        dst += 8;
    }

    // Tail: 4 mono samples holding the last source sample.
    {
        const int16_t s_last = src[N - 1];
        for (int k = 0; k < 4; ++k) {
            dst[0] = s_last;
            dst[1] = s_last;
            dst += 2;
        }
    }

    // Defensive: dst should now point one int16 past the buffer end.
    const ptrdiff_t produced =
        (ptrdiff_t)(dst - s_stereo_buf);
    if (produced != (ptrdiff_t)NANOJS8_TX_AUDIO_STEREO_SAMPLES) {
        ESP_LOGE(TAG, "render: BUG — produced %td samples, expected %d",
                 produced, (int)NANOJS8_TX_AUDIO_STEREO_SAMPLES);
        return ESP_FAIL;
    }

    const int64_t t_end_us = esp_timer_get_time();
    const uint32_t elapsed_ms = (uint32_t)((t_end_us - t_start_us) / 1000);

    s_render_valid = true;
    ESP_LOGI(TAG, "Render OK: %d → %d samples (4× upsample + mono→stereo) "
                  "in %" PRIu32 " ms",
             (int)NANOJS8_JS8_MODULATE_SAMPLE_COUNT,
             (int)NANOJS8_TX_AUDIO_STEREO_SAMPLES,
             elapsed_ms);
    return ESP_OK;
}

// ── Worker task: slot-aligned PTT-keyed TX to USB UAC ────────────────
//
// L7.11d: this replaces L7.11c's plain "stream-now" worker. The new
// sequence:
//   1. Sleep until (next_slot - ptt_on_delay_ms).
//   2. nanojs8_ptt_set(true) — relay closes.
//   3. Sleep ptt_on_delay_ms so audio starts exactly at the slot
//      boundary (relay + radio mode-switch settled).
//   4. Stream chunks (existing loop).
//   5. Sleep ptt_off_delay_ms so the audio tail clears the modem
//      before un-keying.
//   6. nanojs8_ptt_set(false).
//
// Timing values come from the active radio profile, so this works
// across radios with different relay characteristics.
//
// Slot computation: nanojs8_time_millis_today() returns ms-of-day,
// designed to satisfy `% 15000 == 0` at each JS8 Normal slot boundary.
// If the next boundary is too close to safely key PTT before it, we
// skip ahead one slot (15 s) to keep alignment correct.
//
// L7.11e adds Phase 0: the worker now modulates from a caller-
// supplied wire string before the slot wait. This lets ALLCALL /
// COMPOSE pick verbs at the UI layer without baking knowledge into
// the modulator. The modulate step takes ~2 s of CPU (sin() loop),
// so we compute the slot target AFTER modulate completes — otherwise
// we'd undercount the prep budget and risk a missed slot.

typedef struct {
    // L7.11f-fix2f: was char wire[64] which clipped any message above
    // 63 chars even though COMPOSE happily lets the operator type up
    // to 159 (TEXT_BUF_LEN-1) and builds wires of up to 199 chars.
    // 128 here matches the realistic ceiling: 8 frames × 12 chars/body
    // + ~20 chars of to_call + verb + separators = ~116 chars, with
    // headroom. Cost: +64 B per active TX (one transient malloc, freed
    // when the worker task exits — negligible).
    char     wire[128];          // wire-form text to transmit
    char     mycall[12];         // operator's callsign
    char     mygrid[8];          // operator's grid
    uint32_t slot_target_ms;     // ms-of-day at which audio begins.
                                 // Set to 0 by start_transmit and
                                 // computed in worker Phase 0c (after
                                 // modulate completes — see comment
                                 // above).
    uint16_t ptt_on_ms;          // from active radio profile
    uint16_t ptt_off_ms;         // from active radio profile
} tx_plan_t;

// ────────────────────────────────────────────────────────────────────
// L7.11f-fix2: worker task — MicroJS8-style continuous-PTT multi-frame
// ────────────────────────────────────────────────────────────────────
//
// Single-frame (N=1) path is the original L7.11d behavior, unchanged:
// modulate → render → wait slot → PTT on → audio → PTT off. The 20 s
// default watchdog covers the 13.14 s of audio with margin.
//
// Multi-frame (N>1) path follows MicroJS8's pattern:
//   1. Pre-modulate ALL frames into per-frame PSRAM cache buffers
//      BEFORE any slot-scheduling commitment. Encoding finishes ahead
//      of slot boundaries, eliminating the slot-skip we observed when
//      modulate ran in the inter-frame gap (~2 s of work, 1.5 s
//      available). Reference: MicroJS8 EncodeWorker docstring.
//   2. Bump the PTT watchdog to N × 20 000 + 5 000 ms via
//      nanojs8_ptt_set_burst_watchdog_ms() — auto-resets on PTT
//      release.
//   3. Key PTT ONCE. Stream each frame's audio (13.14 s) back-to-back
//      separated by 1.86 s of explicit silence chunks, totalling 15 s
//      per slot. PTT stays asserted across the whole burst — radio
//      sees one continuous transmission rather than N PTT cycles.
//   4. Release PTT once at the end. The burst-watchdog limit is
//      consumed at this point and the default 20 s reapplies.
//
// Strict consecutive-slot rule (MicroJS8 lift): any frame failure —
// modulation error, render error, audio path dead — aborts the WHOLE
// multi-frame TX. We release PTT cleanly and free caches before
// returning. No partial multi-frame messages on the air.
//
// Memory: N × 308 KB per-frame cache + 2.41 MB shared stereo buffer.
// Capped at TX_MAX_FRAMES = 8 (2.46 MB cache), within the ~3 MB free
// PSRAM observed at runtime.

// Helper: stream a known-good buffer through the USB audio path,
// chunked, with per-chunk write timeout. Returns true on completion,
// false if the audio path went dead mid-stream.  `chunks_ok` and
// `chunks_dropped` are accumulated in-place. `label` is used in logs.
static bool stream_buffer(const uint8_t *base, size_t total_bytes,
                          const char *label, uint32_t *chunks_ok,
                          uint32_t *chunks_dropped)
{
    size_t offset = 0;
    while (offset < total_bytes) {
        const size_t remaining = total_bytes - offset;
        const size_t to_send = remaining < (size_t)TX_CHUNK_BYTES
                                 ? remaining
                                 : (size_t)TX_CHUNK_BYTES;
        esp_err_t err = nanojs8_audio_write(base + offset,
                                             (uint32_t)to_send,
                                             TX_WRITE_TIMEOUT_MS);
        if (err == ESP_OK) {
            ++(*chunks_ok);
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "stream(%s): audio endpoint INVALID_STATE at "
                          "offset=%u — aborting stream",
                     label, (unsigned)offset);
            return false;
        } else {
            ++(*chunks_dropped);
            ESP_LOGW(TAG, "stream(%s): audio_write failed at offset=%u "
                          "err=0x%x", label, (unsigned)offset, (int)err);
        }
        offset += to_send;
    }
    return true;
}

// Helper: stream `n_chunks` zero-filled chunks (silence) for use
// between frames in a continuous-PTT burst. Same return convention as
// stream_buffer.
static bool stream_silence_chunks(size_t n_chunks, const char *label,
                                  uint32_t *chunks_ok,
                                  uint32_t *chunks_dropped)
{
    const uint8_t *src = (const uint8_t *)s_silence_chunk;
    for (size_t k = 0; k < n_chunks; ++k) {
        esp_err_t err = nanojs8_audio_write(src,
                                             (uint32_t)TX_CHUNK_BYTES,
                                             TX_WRITE_TIMEOUT_MS);
        if (err == ESP_OK) {
            ++(*chunks_ok);
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "silence(%s): audio endpoint INVALID_STATE at "
                          "chunk %u/%u — aborting",
                     label, (unsigned)k, (unsigned)n_chunks);
            return false;
        } else {
            ++(*chunks_dropped);
            ESP_LOGW(TAG, "silence(%s): audio_write failed at chunk %u "
                          "err=0x%x", label, (unsigned)k, (int)err);
        }
    }
    return true;
}

// Helper: compute the next slot target (millis-of-day) at least
// `lead_ms` from `now_ms_of_day`. Mirrors the Phase 0c math from the
// L7.11d/e worker.
static uint32_t next_slot_target(uint32_t now_ms_of_day, uint32_t lead_ms)
{
    uint32_t target = ((now_ms_of_day / 15000u) + 1u) * 15000u;
    if (target >= 86400000u) target -= 86400000u;
    while (((target + 86400000u - now_ms_of_day) % 86400000u) < lead_ms) {
        target += 15000u;
        if (target >= 86400000u) target -= 86400000u;
    }
    return target;
}

static void tx_worker_task(void *arg)
{
    tx_plan_t plan = *(tx_plan_t *)arg;
    free(arg);  // ownership passed to us

    // L7.11f-fix2d: yield once at task entry, BEFORE the first heavy
    // operation. nanojs8_js8_text_frame_count() below may rebuild the
    // pack-cache (~2 s of regex + LDPC work on a fresh wire string).
    // If a same-priority CPU-bound task (e.g. js8sync finishing a busy
    // slot decode) was running on Core 1 immediately prior, IDLE1's
    // watchdog-reset window may already be most of the way to the
    // 5 s timeout — a yield right here gives IDLE1 a clean reset
    // before pack-cache spends its full 2 s of contiguous CPU.
    vTaskDelay(pdMS_TO_TICKS(1));

    // ── Phase 0: how many frames? ────────────────────────────────────
    const size_t total_frames = nanojs8_js8_text_frame_count(
        plan.wire, plan.mycall, plan.mygrid);
    if (total_frames == 0) {
        ESP_LOGE(TAG, "transmit: text_frame_count returned 0 for '%s' "
                      "— aborting (no PTT yet, safe)", plan.wire);
        atomic_store(&s_active, false);
        s_tx_task_handle = NULL;
        vTaskDeleteWithCaps(NULL);  // L7.11h.2-fix2: matches WithCaps create
    }
    if (total_frames > TX_MAX_FRAMES) {
        ESP_LOGE(TAG, "transmit: '%s' packs to %u frames, exceeds "
                      "TX_MAX_FRAMES=%d — refusing TX (would either "
                      "exceed PSRAM budget or hold PTT > %d s, both "
                      "unsafe). Shorten the message.",
                 plan.wire, (unsigned)total_frames,
                 TX_MAX_FRAMES,
                 TX_MAX_FRAMES * (NANOJS8_PTT_WATCHDOG_MS / 1000));
        atomic_store(&s_active, false);
        s_tx_task_handle = NULL;
        vTaskDeleteWithCaps(NULL);  // L7.11h.2-fix2: matches WithCaps create
    }

    uint32_t total_chunks_ok      = 0;
    uint32_t total_chunks_dropped = 0;
    bool      watchdog_bumped     = false;       // for cleanup safety
    bool      tx_aborted          = false;

    // ────────────────────────────────────────────────────────────────
    // Single-frame fast path (N=1): identical to L7.11d/e behavior.
    // No cache allocation, no watchdog bump, no inter-frame silence.
    // ────────────────────────────────────────────────────────────────
    if (total_frames == 1) {
        // Modulate frame 0 directly into the shared modulator buffer.
        const int64_t t_mod_us = esp_timer_get_time();
        esp_err_t err = nanojs8_js8_modulate_text_frame(
            plan.wire, plan.mycall, plan.mygrid, 0, 1500.0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit: modulate_text_frame(0) failed "
                          "(0x%x) for '%s' — aborting", (int)err, plan.wire);
            goto worker_exit;
        }
        ESP_LOGI(TAG, "Single-frame modulated for '%s' in %" PRIu32 " ms",
                 plan.wire,
                 (uint32_t)((esp_timer_get_time() - t_mod_us) / 1000));

        // Render to stereo.
        err = nanojs8_tx_audio_render_from_modulator();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit: render_from_modulator failed (0x%x) "
                          "— aborting", (int)err);
            goto worker_exit;
        }

        // Compute slot target NOW.
        const uint32_t lead = (uint32_t)plan.ptt_on_ms + 100u;
        plan.slot_target_ms =
            next_slot_target(nanojs8_time_millis_today(), lead);
    } else {
        // ────────────────────────────────────────────────────────────
        // Multi-frame (N>1): MicroJS8-style continuous-PTT burst.
        //
        // L7.14-fix11 Path B: just-in-time modulation. Previously we
        // pre-modulated ALL N frames into N separate PSRAM caches
        // (N × 315 KB), which capped practical multi-frame TX at ~5
        // frames before PSRAM exhausted and the worker silently
        // aborted. Now we modulate one frame at a time, reusing the
        // single shared modulator buffer that's already allocated at
        // boot. Frame 0 is modulated upfront (before PTT); frames
        // 1..N-1 are modulated during the 1.86 s inter-frame silence
        // (modulate ~1 s + render ~85 ms easily fits — see the loop
        // below). Net result: multi-frame TX uses zero additional
        // PSRAM regardless of frame count, so all 8 frames work
        // reliably even with the runtime-PSRAM-tight state.
        // ────────────────────────────────────────────────────────────
        ESP_LOGI(TAG, "Multi-frame TX: %u frames will be transmitted as "
                      "a single continuous-PTT burst (just-in-time "
                      "modulation per frame, single shared modulator "
                      "buffer)…",
                 (unsigned)total_frames);

        // Modulate frame 0 now (before PTT key). Pack cache will be
        // hot from the synchronous pre-flight in transmit_text(), so
        // this call returns in <100 ms even for a fresh wire.
        const int64_t t_premod_us = esp_timer_get_time();
        // L7.11f-fix2d: yield before the heavy modulate call to give
        // IDLE1 a fresh watchdog window. Same rationale as the old
        // pre-modulate loop; preserved here for the single-frame-at-a-
        // time path.
        vTaskDelay(pdMS_TO_TICKS(1));
        esp_err_t err = nanojs8_js8_modulate_text_frame(
            plan.wire, plan.mycall, plan.mygrid, 0, 1500.0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit: modulate_text_frame(0/%u) failed "
                          "(0x%x) for '%s' — aborting (no PTT yet, safe)",
                     (unsigned)total_frames, (int)err, plan.wire);
            goto worker_exit;
        }
        ESP_LOGI(TAG, "Modulated frame 1/%u in %" PRIu32 " ms "
                      "(modulator buffer ready, %d B)",
                 (unsigned)total_frames,
                 (uint32_t)((esp_timer_get_time() - t_premod_us) / 1000),
                 NANOJS8_JS8_MODULATE_BUFFER_BYTES);

        // Render frame 0 from the modulator's buffer to the shared
        // stereo output. Modulator buffer already contains frame 0
        // (no load_samples needed — modulate just wrote it there).
        err = nanojs8_tx_audio_render_from_modulator();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit: render_from_modulator(0) failed "
                          "(0x%x) — aborting", (int)err);
            goto worker_exit;
        }

        // Compute slot target NOW (after frame-0 prep is done).
        const uint32_t lead = (uint32_t)plan.ptt_on_ms + 100u;
        plan.slot_target_ms =
            next_slot_target(nanojs8_time_millis_today(), lead);

        // Bump PTT watchdog for the continuous burst. Done LAST in
        // the prep phase so all the things that could fail have been
        // ruled out — we never want to bump the watchdog and then
        // abort before PTT, leaving the bump untouched. (The bump is
        // also auto-cleared on the next ptt_set(false), so the worst
        // case is still safe.)
        const uint32_t wdt_budget_ms =
            (uint32_t)total_frames * (uint32_t)NANOJS8_PTT_WATCHDOG_MS
            + (uint32_t)TX_BURST_WDT_GRACE_MS;
        nanojs8_ptt_set_burst_watchdog_ms(wdt_budget_ms);
        watchdog_bumped = true;
        ESP_LOGI(TAG, "Continuous-PTT burst armed: %u frames × %d ms "
                      "+ %d ms grace = %" PRIu32 " ms PTT watchdog limit",
                 (unsigned)total_frames, NANOJS8_PTT_WATCHDOG_MS,
                 TX_BURST_WDT_GRACE_MS, wdt_budget_ms);
    }

    // ────────────────────────────────────────────────────────────────
    // Common path from here: slot wait → PTT on → settle → stream.
    // Frame 0's stereo buffer is already rendered.
    // ────────────────────────────────────────────────────────────────

    // Phase A: sleep until ptt_on_ms before the slot boundary.
    {
        const uint32_t now_ms     = nanojs8_time_millis_today();
        const uint32_t ptt_key_ms = (plan.slot_target_ms + 86400000u
                                      - (uint32_t)plan.ptt_on_ms)
                                     % 86400000u;
        uint32_t wait_ms = (ptt_key_ms + 86400000u - now_ms) % 86400000u;
        if (wait_ms > 86400000u / 2u) wait_ms = 0;

        ESP_LOGI(TAG, "TX plan: %u frame(s), first slot at "
                      "%02u:%02u:%02u.%03u (in %" PRIu32 " ms), "
                      "PTT on/off=%u/%u ms%s",
                 (unsigned)total_frames,
                 (unsigned)((plan.slot_target_ms / 3600000u) % 24u),
                 (unsigned)((plan.slot_target_ms / 60000u) % 60u),
                 (unsigned)((plan.slot_target_ms / 1000u) % 60u),
                 (unsigned)(plan.slot_target_ms % 1000u),
                 (uint32_t)((plan.slot_target_ms + 86400000u - now_ms)
                              % 86400000u),
                 (unsigned)plan.ptt_on_ms, (unsigned)plan.ptt_off_ms,
                 (total_frames > 1) ? " (continuous-PTT burst)" : "");

        if (wait_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    // Phase B: key PTT.
    {
        const int64_t t_key_us = esp_timer_get_time();
        esp_err_t err = nanojs8_ptt_set(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit: ptt_set(true) failed (0x%x) — "
                          "aborting", (int)err);
            tx_aborted = true;
            goto worker_exit;
        }
        ESP_LOGI(TAG, "PTT keyed at millis_today=%" PRIu32 " "
                      "(%.3f ms from slot target)",
                 nanojs8_time_millis_today(),
                 (double)(esp_timer_get_time() - t_key_us) / 1000.0);
    }

    // Phase C: ptt_on settle delay.
    vTaskDelay(pdMS_TO_TICKS(plan.ptt_on_ms));
    {
        const uint32_t now_at_audio = nanojs8_time_millis_today();
        const int32_t skew_raw =
            (int32_t)((now_at_audio + 86400000u - plan.slot_target_ms)
                        % 86400000u);
        const int32_t skew_ms =
            skew_raw > 86400000 / 2 ? skew_raw - 86400000 : skew_raw;
        ESP_LOGI(TAG, "Audio start at millis_today=%" PRIu32 " "
                      "(skew %+" PRId32 " ms vs first slot)",
                 now_at_audio, skew_ms);
    }

    // Phase D: stream each frame's audio, with inter-frame silence
    // for continuous-PTT bursts. Frame 0 is already in s_stereo_buf;
    // frames 1..N-1 are loaded+rendered between streams.
    bool audio_path_dead = false;
    const int64_t t_audio_start_us = esp_timer_get_time();
    for (size_t i = 0; i < total_frames && !audio_path_dead; ++i) {
        // Stream frame i audio.
        // L7.11f-fix2-build1: 32 not 24 — GCC's -Werror=format-truncation
        // doesn't track that total_frames ≤ TX_MAX_FRAMES=8, so it
        // assumes %u could be the full 10-char UINT32_MAX. Buffer must
        // fit "frame 4294967295/4294967295\0" = 28 bytes worst case.
        char label[32];
        snprintf(label, sizeof(label), "frame %u/%u",
                 (unsigned)(i + 1), (unsigned)total_frames);
        const int64_t t_frame_start = esp_timer_get_time();
        if (!stream_buffer((const uint8_t *)s_stereo_buf,
                            (size_t)NANOJS8_TX_AUDIO_BUFFER_BYTES,
                            label,
                            &total_chunks_ok,
                            &total_chunks_dropped)) {
            audio_path_dead = true;
            break;
        }
        ESP_LOGI(TAG, "Frame %u/%u streamed in %" PRIu32 " ms",
                 (unsigned)(i + 1), (unsigned)total_frames,
                 (uint32_t)((esp_timer_get_time() - t_frame_start)
                              / 1000));

        // If not the last frame, prep frame i+1 then stream silence.
        if (i + 1 < total_frames) {
            // L7.14-fix11 Path B: modulate frame i+1 directly into the
            // modulator's shared buffer (no separate cache needed). The
            // 1.86 s inter-frame silence covers modulate (~1 s with hot
            // pack cache) + render (~85 ms) easily. Audio output is
            // briefly starved while these run, but we're entering the
            // explicit silence period anyway, so the USB queue drain
            // to silence is invisible.
            const int64_t t_modulate_us = esp_timer_get_time();
            esp_err_t err = nanojs8_js8_modulate_text_frame(
                plan.wire, plan.mycall, plan.mygrid, i + 1, 1500.0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "transmit: modulate_text_frame(%u) failed "
                              "(0x%x) mid-burst — aborting whole TX",
                         (unsigned)(i + 1), (int)err);
                audio_path_dead = true;
                tx_aborted = true;
                break;
            }
            const uint32_t modulate_ms =
                (uint32_t)((esp_timer_get_time() - t_modulate_us) / 1000);

            const int64_t t_render_us = esp_timer_get_time();
            err = nanojs8_tx_audio_render_from_modulator();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "transmit: render(%u) failed (0x%x) "
                              "mid-burst — aborting whole TX",
                         (unsigned)(i + 1), (int)err);
                audio_path_dead = true;
                tx_aborted = true;
                break;
            }
            const uint32_t render_ms =
                (uint32_t)((esp_timer_get_time() - t_render_us) / 1000);

            // Stream inter-frame silence.
            // Same buffer-size reasoning as `label` above; the UTF-8
            // arrow is 3 bytes which only makes things tighter.
            char silence_label[32];
            snprintf(silence_label, sizeof(silence_label),
                     "gap %u→%u",
                     (unsigned)(i + 1), (unsigned)(i + 2));
            const int64_t t_silence_us = esp_timer_get_time();
            if (!stream_silence_chunks((size_t)TX_INTERFRAME_SILENCE_CHUNKS,
                                        silence_label,
                                        &total_chunks_ok,
                                        &total_chunks_dropped)) {
                audio_path_dead = true;
                break;
            }
            ESP_LOGI(TAG, "Inter-frame gap %u→%u: modulate %" PRIu32 " ms "
                          "+ render %" PRIu32 " ms + silence %" PRIu32 " ms "
                          "(%d chunks)",
                     (unsigned)(i + 1), (unsigned)(i + 2),
                     modulate_ms, render_ms,
                     (uint32_t)((esp_timer_get_time() - t_silence_us)
                                  / 1000),
                     TX_INTERFRAME_SILENCE_CHUNKS);
        }
    }

    const uint32_t audio_wall_ms =
        (uint32_t)((esp_timer_get_time() - t_audio_start_us) / 1000);

    // Phase E: ptt_off tail delay (skip if audio is dead — we want PTT
    // off as fast as possible).
    if (!audio_path_dead) {
        vTaskDelay(pdMS_TO_TICKS(plan.ptt_off_ms));
    }

    // Phase F: release PTT. ptt_set(false) also consumes any active
    // burst-watchdog limit, restoring the default 20 s ceiling for
    // the next assert. After this point watchdog_bumped is effectively
    // already cleared — but we keep the flag to suppress the explicit
    // clear in worker_exit on the happy path.
    {
        esp_err_t err = nanojs8_ptt_set(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit: ptt_set(false) failed (0x%x)",
                     (int)err);
        }
        watchdog_bumped = false;  // consumed by the release above
    }

    ESP_LOGI(TAG, "TX done: %u frame(s), %" PRIu32 " chunks OK, "
                  "%" PRIu32 " dropped, audio wall=%" PRIu32 " ms%s",
             (unsigned)total_frames,
             total_chunks_ok, total_chunks_dropped, audio_wall_ms,
             audio_path_dead ? " (AUDIO PATH DIED MID-BURST)" : "");

    if (audio_path_dead) tx_aborted = true;

worker_exit:
    // If we bumped the watchdog but never keyed PTT (something failed
    // between bump and key), explicitly clear the bump so the next
    // legitimate single-frame TX isn't running under an extended
    // ceiling. On the happy path watchdog_bumped is already false
    // (cleared after ptt_set(false)).
    if (watchdog_bumped) {
        ESP_LOGW(TAG, "Worker exiting with watchdog bumped but no PTT "
                      "release happened — explicitly clearing bump");
        nanojs8_ptt_set_burst_watchdog_ms(0);
    }

    if (tx_aborted) {
        ESP_LOGE(TAG, "Multi-frame TX aborted (strict consecutive-slot "
                      "rule): partial message NOT re-attempted");
    }

    // Release the active flag last, after PTT is fully down. A caller
    // who polls is_active() and triggers a new TX will see the line
    // open before they start.
    s_tx_task_handle = NULL;
    atomic_store(&s_active, false);

    // L7.11h.2-fix2: log how much of the 16 KB PSRAM-resident task
    // stack was actually used. This is the runtime peak — includes
    // pack-cache regex + LDPC work, modulation, all of it. Compare
    // against TX_TASK_STACK to confirm headroom (or to inform a
    // future stack-size reduction if headroom is large).
    const UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
        "tx_worker: stack high-water = %u B free of %u B "
        "(used %u B; stack lives in PSRAM via WithCaps)",
        (unsigned)(hwm_words * sizeof(StackType_t)),
        (unsigned)TX_TASK_STACK,
        (unsigned)(TX_TASK_STACK - hwm_words * sizeof(StackType_t)));

    vTaskDeleteWithCaps(NULL);  // L7.11h.2-fix2: matches WithCaps create
}

esp_err_t nanojs8_tx_audio_transmit_text(const char *wire_text)
{
    if (!s_init_ok) {
        ESP_LOGE(TAG, "transmit_text: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wire_text || wire_text[0] == '\0') {
        ESP_LOGE(TAG, "transmit_text: NULL or empty wire");
        return ESP_ERR_INVALID_ARG;
    }
    const size_t wire_len = strlen(wire_text);
    if (wire_len >= sizeof(((tx_plan_t *)0)->wire)) {
        ESP_LOGE(TAG, "transmit_text: wire too long (%u, max %u)",
                 (unsigned)wire_len,
                 (unsigned)(sizeof(((tx_plan_t *)0)->wire) - 1));
        return ESP_ERR_INVALID_ARG;
    }

    // Precondition 1: UTC must be set. Without it we can't compute the
    // slot boundary, and an unaligned signal is invalid JS8.
    if (!nanojs8_time_is_set()) {
        ESP_LOGE(TAG, "transmit_text: UTC not set — cannot slot-align "
                      "(enter UTC via SETUP row 6 first)");
        return ESP_ERR_INVALID_STATE;
    }

    // Precondition 2: an active radio profile is required for PTT
    // timing. Should always be present in a configured station, but
    // belt-and-suspenders.
    const nanojs8_radio_profile_t *profile = nanojs8_radio_get_active();
    if (!profile) {
        ESP_LOGE(TAG, "transmit_text: no active radio profile");
        return ESP_ERR_INVALID_STATE;
    }

    // Precondition 3: station config (callsign + grid) needed for the
    // modulate step. We snapshot them into the plan so the worker
    // task is self-contained — if the operator opens SETUP and edits
    // the callsign during a TX, this transmission keeps the original
    // call (intentional: changing mid-TX would produce a malformed
    // packet).
    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (!cfg || cfg->callsign[0] == '\0') {
        ESP_LOGE(TAG, "transmit_text: empty callsign in config");
        return ESP_ERR_INVALID_STATE;
    }

    // L7.14-fix9: synchronous frame-count pre-flight. Previously this
    // check lived only in the worker task at Phase 0, by which time
    // transmit_text had already returned ESP_OK and the caller (e.g.
    // COMPOSE's fire_send) had logged the message to DIRECTED. A too-
    // long wire silently aborted with no PTT and no UI feedback —
    // operator pressed SEND, nothing happened, no error banner.
    //
    // Pulling the check into the caller's task means transmit_text can
    // return a specific error code (ESP_ERR_INVALID_SIZE for too-many-
    // frames, ESP_ERR_INVALID_ARG for unpackable input) and fire_send
    // can show a tailored banner. The DIRECTED log only fires on real
    // ESP_OK acceptance.
    //
    // Cost: nanojs8_js8_text_frame_count caches the pack result by
    // (text,call,grid). First call for a given wire may take ~2 s of
    // regex + LDPC work to build the cache; subsequent calls — and the
    // worker's defense-in-depth re-check at Phase 0 — are O(1) hash
    // lookups. Acceptable stall on a SEND button press.
    const size_t preflight_frames = nanojs8_js8_text_frame_count(
        wire_text, cfg->callsign, cfg->grid);
    if (preflight_frames == 0) {
        ESP_LOGE(TAG, "transmit_text: pack failed for '%s' — refusing TX",
                 wire_text);
        return ESP_ERR_INVALID_ARG;
    }
    if (preflight_frames > TX_MAX_FRAMES) {
        ESP_LOGE(TAG,
            "transmit_text: '%s' packs to %u frames, exceeds "
            "TX_MAX_FRAMES=%d — refusing TX. Operator should shorten "
            "the message body.",
            wire_text, (unsigned)preflight_frames, TX_MAX_FRAMES);
        return ESP_ERR_INVALID_SIZE;
    }

    // L7.14-fix11 Path B: with just-in-time modulation, multi-frame TX
    // no longer pre-allocates per-frame caches — the single modulator
    // buffer (315 KB, already allocated at boot) is reused for each
    // frame in sequence. So the worker's runtime PSRAM need is now
    // independent of frame count.
    //
    // We keep a sanity check for catastrophic PSRAM exhaustion — if
    // total free is below the per-frame buffer size, something is
    // very wrong (other component leak, OOM in another subsystem)
    // and TX should refuse rather than wedge mid-burst.
    const size_t per_frame_cache_bytes = NANOJS8_JS8_MODULATE_BUFFER_BYTES;
    const size_t free_psram_bytes      = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram_bytes < per_frame_cache_bytes) {
        ESP_LOGE(TAG,
            "transmit_text: PSRAM critically low (%u B free, %u B sanity "
            "minimum) — refusing TX. Some other subsystem is leaking or "
            "OOM'd; expect issues beyond just TX.",
            (unsigned)free_psram_bytes, (unsigned)per_frame_cache_bytes);
        return ESP_ERR_NO_MEM;
    }

    // Precondition 4: atomic single-instance lock. Only one TX at a
    // time — refuse if a previous one is still in flight.
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_active, &expected, true)) {
        ESP_LOGW(TAG, "transmit_text: already active, ignoring");
        return ESP_ERR_INVALID_STATE;
    }

    // Allocate the plan struct on the heap so it can outlive this
    // function. The worker takes ownership and frees it.
    tx_plan_t *plan = (tx_plan_t *)malloc(sizeof(tx_plan_t));
    if (!plan) {
        atomic_store(&s_active, false);
        ESP_LOGE(TAG, "transmit_text: out of memory for tx_plan");
        return ESP_ERR_NO_MEM;
    }
    memset(plan, 0, sizeof(*plan));

    // Copy wire + station snapshot. strncpy + explicit NUL belt-and-
    // suspenders against truncation; we already bounded wire_len above
    // so the strncpy will never truncate, but cfg->callsign / cfg->grid
    // are full-sized fields and may not fit if their max changed.
    strncpy(plan->wire,   wire_text,     sizeof(plan->wire)   - 1);
    strncpy(plan->mycall, cfg->callsign, sizeof(plan->mycall) - 1);
    strncpy(plan->mygrid, cfg->grid,     sizeof(plan->mygrid) - 1);
    plan->slot_target_ms = 0;  // computed inside worker Phase 0c
    plan->ptt_on_ms      = profile->ptt_on_delay_ms;
    plan->ptt_off_ms     = profile->ptt_off_delay_ms;

    // L7.11h.2-fix2: diagnostic snapshot of heap state right before
    // task creation. After C2/C3 added more internal-RAM-resident
    // static/BSS state, the 16 KB TX task stack no longer fits in any
    // single contiguous internal-RAM block (160 KB region heavily
    // fragmented by ~15 task stacks at boot; 32 KB DRAM region likely
    // partially used too). The PSRAM-stack workaround below should
    // succeed regardless, but logging the numbers tells us how close
    // to the wire we were and feeds future memory-budget decisions.
    const size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psr_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psr_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
        "transmit_text: pre-task heap — internal free=%u largest=%u, "
        "PSRAM free=%u largest=%u; requesting %u-byte stack in PSRAM",
        (unsigned)int_free, (unsigned)int_largest,
        (unsigned)psr_free, (unsigned)psr_largest,
        (unsigned)TX_TASK_STACK);

    // L7.11h.2-fix2: use xTaskCreatePinnedToCoreWithCaps to put the
    // 16 KB task stack in PSRAM. The TCB (~200 B) stays in internal
    // RAM regardless — it's small enough to fit in whatever
    // fragmentation pockets remain. The stack is where the size
    // pressure is, and PSRAM has megabytes free.
    //
    // Performance note: PSRAM stack access is ~2× slower than internal
    // RAM. Acceptable here because the entire 13.14 s audio buffer is
    // pre-rendered to a stereo PSRAM buffer BEFORE PTT activates — so
    // any stack-bound work (pack-cache regex, LDPC, modulation) all
    // happens in the non-realtime Phase 0 prep window. Once PTT goes
    // high, audio streaming is from the pre-rendered buffer via DMA,
    // not from stack operations.
    //
    // Stack allocated with WithCaps MUST be freed with vTaskDeleteWithCaps —
    // see the three vTaskDeleteWithCaps(NULL) sites inside tx_worker_task.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        tx_worker_task, TX_TASK_NAME, TX_TASK_STACK, plan,
        TX_TASK_PRIORITY, &s_tx_task_handle, TX_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        free(plan);
        atomic_store(&s_active, false);
        ESP_LOGE(TAG,
            "transmit_text: xTaskCreatePinnedToCoreWithCaps failed "
            "(rc=%d) — even PSRAM allocation declined; "
            "PSRAM free=%u largest=%u (needed %u)",
            (int)rc,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
            (unsigned)TX_TASK_STACK);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool nanojs8_tx_audio_is_active(void)
{
    return atomic_load(&s_active);
}

bool nanojs8_tx_audio_is_initialized(void)
{
    // s_init_ok is monotonic (set true in init(), never cleared). A
    // plain read is correct here: the UI calls this from the LVGL /
    // input task, and the only writer is the boot-time self-test on
    // a different core — by the time the UI is up and a key is
    // pressed, that write is long-since committed to memory.
    return s_init_ok;
}

// ── Self-test (Subtest D) ────────────────────────────────────────────

bool nanojs8_tx_audio_self_test(void)
{
    ESP_LOGI(TAG, "Self-test D: starting (init + render + sanity)");

    // D.1: ensure init succeeded.
    if (nanojs8_tx_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "Self-test D: init FAILED");
        return false;
    }

    // D.2: render. The encoder/modulator self-test (subtests A/B/C) just
    //      finished and the modulator buffer is freshly populated, so
    //      our render-from-modulator picks up real CPFSK data.
    if (nanojs8_tx_audio_render_from_modulator() != ESP_OK) {
        ESP_LOGE(TAG, "Self-test D: render FAILED");
        return false;
    }

    // D.3: mono→stereo correctness. Every odd-indexed sample (R channel)
    //      must equal the preceding even-indexed sample (L channel).
    //      Check every frame — cheap with a tight loop.
    for (size_t i = 0; i < (size_t)NANOJS8_TX_AUDIO_STEREO_SAMPLES; i += 2) {
        if (s_stereo_buf[i] != s_stereo_buf[i + 1]) {
            ESP_LOGE(TAG, "Self-test D: L!=R at frame %u "
                          "(L=%d R=%d)",
                     (unsigned)(i / 2),
                     (int)s_stereo_buf[i], (int)s_stereo_buf[i + 1]);
            return false;
        }
    }

    // D.4: silent start-delay region. The first 6000 mono samples
    //      (500 ms × 12 kHz) from the modulator are zero, so the first
    //      6000 × 4 = 24,000 mono outputs are zero, which in stereo
    //      is the first 48,000 int16 samples.
    const size_t silent_end_samples =
        6000u * NANOJS8_TX_AUDIO_UPSAMPLE_RATIO * NANOJS8_TX_AUDIO_CHANNELS;
    for (size_t i = 0; i < silent_end_samples; ++i) {
        if (s_stereo_buf[i] != 0) {
            ESP_LOGE(TAG, "Self-test D: delay region not silent at i=%u "
                          "(s=%d)",
                     (unsigned)i, (int)s_stereo_buf[i]);
            return false;
        }
    }

    // D.5: peak amplitude. Linear interpolation between two int16 values
    //      cannot exceed the larger of the two endpoints in magnitude, so
    //      the peak of the upsampled signal is bounded above by the source
    //      peak (which subtest C already pinned at 16384). We allow ±1
    //      slack for integer-divide rounding.
    int32_t peak = 0;
    for (size_t i = silent_end_samples;
         i < (size_t)NANOJS8_TX_AUDIO_STEREO_SAMPLES; ++i) {
        int32_t v = (int32_t)s_stereo_buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    if (peak < 16383 || peak > 16384) {
        ESP_LOGE(TAG, "Self-test D: peak=%d outside [16383, 16384]",
                 (int)peak);
        return false;
    }

    ESP_LOGI(TAG, "Self-test D: PASS — %d stereo samples (%d frames), "
                  "L=R verified, delay region silent, peak=%d",
             (int)NANOJS8_TX_AUDIO_STEREO_SAMPLES,
             (int)(NANOJS8_TX_AUDIO_STEREO_SAMPLES
                   / NANOJS8_TX_AUDIO_CHANNELS),
             (int)peak);
    return true;
}
