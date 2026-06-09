/*
 * tx_audio.h — L7.11c JS8 TX audio pipeline
 * ==========================================
 *
 * Bridges the gap between the modulator (12 kHz mono int16 in PSRAM)
 * and the USB UAC TX endpoint (48 kHz stereo int16). Pre-renders the
 * upsampled-and-duplicated stream into a second PSRAM buffer at boot,
 * then streams chunks through nanojs8_audio_write() under a one-shot
 * worker task when test-TX is triggered.
 *
 * Memory cost (one-time, held until reboot):
 *   - 12 kHz mono source   : 315,360 B  (owned by nanojs8_js8_codec/modulate)
 *   - 48 kHz stereo dest   : 2,522,880 B (owned here)
 *   - TOTAL                : ~2.83 MB PSRAM
 *
 * No allocations during streaming. nanojs8_audio_write reads chunks
 * directly out of the pre-rendered buffer.
 *
 * Important: this is the L7.11c TEST path. It does NOT key PTT. Audio
 * flows to DigiRig (and through to the radio's audio-in) but the radio
 * stays in RX. Verification = radio's audio meter jumps for ~13 s.
 *
 * License: GPL-3.0
 */
#pragma once

#include "esp_err.h"
#include "js8_codec.h"  // NANOJS8_JS8_MODULATE_SAMPLE_COUNT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Buffer geometry (compile-time constants) ─────────────────────────

/// Output sample rate (USB UAC TX endpoint negotiates this).
#define NANOJS8_TX_AUDIO_SAMPLE_RATE     48000

/// 4× upsample from the 12 kHz modulator output.
#define NANOJS8_TX_AUDIO_UPSAMPLE_RATIO  4

/// Stereo (2-channel interleaved L/R/L/R…) — the negotiated TX format.
/// JS8 audio is mono so we duplicate. Many rig interfaces only use one
/// channel; sending the signal on both is the safe default.
#define NANOJS8_TX_AUDIO_CHANNELS        2

/// Mono samples after upsampling: 157,680 × 4 = 630,720.
#define NANOJS8_TX_AUDIO_MONO_SAMPLES    \
    (NANOJS8_JS8_MODULATE_SAMPLE_COUNT * NANOJS8_TX_AUDIO_UPSAMPLE_RATIO)

/// Stereo SAMPLES (not frames) — 630,720 × 2 = 1,261,440.
#define NANOJS8_TX_AUDIO_STEREO_SAMPLES  \
    (NANOJS8_TX_AUDIO_MONO_SAMPLES * NANOJS8_TX_AUDIO_CHANNELS)

/// Total buffer size in bytes (int16 PCM): 1,261,440 × 2 = 2,522,880.
#define NANOJS8_TX_AUDIO_BUFFER_BYTES    \
    (NANOJS8_TX_AUDIO_STEREO_SAMPLES * (int)sizeof(int16_t))

// ── Public API ───────────────────────────────────────────────────────

/**
 * Allocate the pre-render buffer in PSRAM. Idempotent. Must succeed
 * before any other function in this header can be called. The buffer
 * is zero-initialized — silent until render is called.
 *
 * Returns ESP_OK or ESP_ERR_NO_MEM if PSRAM can't satisfy the ~2.41 MB
 * request (log shows free-PSRAM detail).
 */
esp_err_t nanojs8_tx_audio_init(void);

/**
 * Pre-render the modulator's 12 kHz mono buffer into our 48 kHz stereo
 * buffer. Linear 4× upsample, then L=R duplication.
 *
 * Precondition: nanojs8_js8_modulate_heartbeat() (or similar) must have
 * been called recently — we read from its internal buffer. If the
 * modulator has not produced a frame, the source is silent and our
 * output will be silent (no error, just zero audio).
 *
 * Idempotent in the sense that calling twice with the same modulator
 * state produces the same output buffer. Safe to call repeatedly.
 *
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if tx_audio not init,
 *         ESP_FAIL if modulator buffer is not available.
 */
esp_err_t nanojs8_tx_audio_render_from_modulator(void);

/**
 * Trigger a slot-aligned on-air transmission: spawn a worker task that
 * waits for the next JS8 Normal slot boundary, keys PTT, streams the
 * pre-rendered audio buffer, then releases PTT. Returns immediately
 * (fire-and-forget).
 *
 * Refuses (returns ESP_ERR_INVALID_STATE) if:
 *   - tx_audio not initialized
 *   - a transmit is already in progress
 *   - UTC is not set (no way to slot-align)
 *   - no active radio profile (PTT method unknown)
 *
 * Timing (driven by the active radio profile):
 *   - PTT asserts (slot_boundary − ptt_on_delay_ms)
 *   - Audio streaming begins AT the slot boundary
 *   - Audio runs 13.14 s (500 ms silent prefix + 12.64 s of JS8 data)
 *   - PTT releases (audio_end + ptt_off_delay_ms)
 *
 * For Xiegu G90 + DigiRig (the typical profile) this is 300 ms key →
 * 13140 ms audio → 200 ms tail = 13.64 s of keyed RF, comfortably
 * inside the 15-second slot and the 20-second PTT watchdog.
 *
 * THE RADIO WILL TRANSMIT. The slot-aligned audio output is real
 * JS8 — any station tuned to the band will hear it. Make sure the
 * radio is tuned to a JS8 channel (typically 7.078 MHz USB for 40m)
 * and that the audio level / RF power are appropriate for your
 * operating environment.
 *
 * The `wire_text` is the on-air message body WITHOUT the auto-prefixed
 * "<from>: " envelope. The encoder adds that automatically. Examples:
 *
 *   "@HB HEARTBEAT EN83"       — broadcast heartbeat
 *   "@ALLCALL QUERY MSGS"      — ask all stations for held messages
 *   "CQ CQ CQ EN83"            — calling for any station
 *   "K1ABC SNR?"               — directed signal-report request
 *
 * Long messages that require multiple JS8 frames are NOT yet supported
 * — only the first frame is transmitted. The encoder logs a warning
 * in that case.
 *
 * The callsign and grid are read from the current config at the time
 * of this call and snapshotted into the worker — operator edits to
 * the station's call mid-TX do not affect the in-flight packet.
 *
 * @param wire_text wire-form message to transmit (max 127 bytes + NUL).
 * @return ESP_OK if task started, ESP_ERR_INVALID_ARG for bad wire,
 *         ESP_ERR_INVALID_STATE on precondition failure, ESP_FAIL on
 *         task-spawn failure, ESP_ERR_NO_MEM on alloc failure.
 */
esp_err_t nanojs8_tx_audio_transmit_text(const char *wire_text);

/**
 * Query whether a transmission is currently in progress (slot wait,
 * PTT key delay, audio streaming, or PTT release delay — any of
 * these). Used by the UI to grey out the trigger key, refuse PTT
 * toggle, show a "TX" indicator, etc.
 */
bool nanojs8_tx_audio_is_active(void);

/**
 * Standalone self-test (Subtest D): allocates buffer, renders from the
 * modulator's current contents, verifies:
 *   - sample count = NANOJS8_TX_AUDIO_STEREO_SAMPLES
 *   - L channel == R channel everywhere (correct mono→stereo)
 *   - first 4 samples match modulator's first sample (delay region zero)
 *   - peak abs amplitude preserved (≤ source peak; linear interp can't
 *     increase peak between two same-sign samples)
 *   - start-delay region (first 24,000 stereo frames) is all zeros
 *
 * Returns true on PASS. Logs details either way.
 *
 * Designed to be called from nanojs8_js8_encode_self_test() after
 * subtest C completes — the modulator buffer is freshly rendered then,
 * so our render-from-modulator picks up real data.
 */
bool nanojs8_tx_audio_self_test(void);

/**
 * L7.13-fix3: precondition probe for the UI layer.
 *
 * Returns true iff nanojs8_tx_audio_init() ran successfully (the
 * PSRAM stereo buffer is allocated and ready to render into). Used by
 * screen_allcall and screen_compose to give the operator a specific
 * error message ("TX self-test failed at boot — reboot device") rather
 * than the generic ESP_ERR_INVALID_STATE that transmit_text returns
 * for any of several distinct precondition failures (init, UTC, radio
 * profile, callsign, already-active).
 *
 * Cheap (single bool read), thread-safe (s_init_ok is set once at boot
 * and never cleared; no memory barrier needed).
 */
bool nanojs8_tx_audio_is_initialized(void);

#ifdef __cplusplus
}
#endif
