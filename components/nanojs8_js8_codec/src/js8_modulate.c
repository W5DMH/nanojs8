/*
 * js8_modulate.c — L7.11b JS8 Normal CPFSK modulator
 * ===================================================
 * Turns the 79 tones from js8_encode.cpp into 12 kHz int16 mono PCM
 * audio in a pre-allocated PSRAM buffer. No heap allocations during
 * TX; the buffer is allocated exactly once at boot.
 *
 * Math (mirrors gfsk8::modulate in api.cpp lines 89-130 — verified to
 * interop with real-world JS8 traffic our RX side has been decoding):
 *
 *   For each of 79 symbols (after 500 ms silent start delay):
 *     freq = audio_freq_hz + tone × 6.25 Hz
 *     dphi = 2π × freq / 12000
 *     For each of 1920 samples in the symbol:
 *       sample = (int16)(AMP × sin(phase))
 *       phase  += dphi
 *       wrap phase to (-π, π] for numerical stability
 *
 * Phase is double (52-bit mantissa) to keep accumulation accurate
 * across 157,680 samples ≈ 82,000 rad ≈ 13,000 cycles. Single-precision
 * would drift noticeably toward the end of a 13-second TX.
 *
 * Despite the "GFSK8" project name, gfsk8's reference modulator does
 * NOT apply Gaussian pulse shaping — symbol transitions are hard
 * frequency steps with continuous phase (CPFSK). Whatever shaping
 * JS8Call applies, the gfsk8 implementation is interoperable; our
 * RX has been decoding real JS8 stations using this same approach.
 *
 * License: GPL-3.0 (inherits from gfsk8-modem-clean per ATTRIBUTION.md)
 */

#include "js8_codec.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"     // L7.14-fix5: EXT_RAM_BSS_ATTR (PSRAM placement)

static const char *TAG = "js8mod";

// ── JS8 Normal modulation constants ─────────────────────────────────
// All exact. SR/SPS = 12000/1920 = 6.25 Hz tone spacing.

#define SPS               1920          // samples per symbol
#define NSYM              79            // tones per frame (Costas-bracketed)
#define DELAY_SAMPLES     6000          // 500 ms × 12 kHz
#define DATA_START        DELAY_SAMPLES // first non-silent sample index
#define DATA_SAMPLES      (NSYM * SPS)  // 151,680
// _SAMPLE_COUNT and _SAMPLE_RATE are exposed via js8_codec.h

// Output amplitude: int16 peak. 16384 = -6 dBFS, giving 6 dB of headroom
// against potential downstream gain stages. AC-coupled audio in / out at
// DigiRig means absolute level isn't critical, but staying well below
// full scale avoids clipping in any digital path before the codec.
#define AMP               16384.0

#define TWO_PI            6.283185307179586
#define PI_D              3.141592653589793

// ── State ───────────────────────────────────────────────────────────
// Single global PSRAM buffer. Allocated at init, never freed. The TX
// pipeline owns it for the lifetime of the program — no second TX can
// be queued while one is in progress, so single-buffer ownership is
// safe.
static int16_t *s_samples            = NULL;
static bool     s_modulator_init_ok  = false;

// L7.14-fix4: sine LUT for the modulator inner loop. Replaces ~151,680
// runtime double-precision sin() calls with a table lookup + 8-bit
// linear interpolation, cutting TX render time from ~2 s to ~50 ms.
//
// The table is PRE-SCALED to AMP (16384), so entries ARE int16_t
// output values — no per-sample multiply is needed at runtime. This
// also means precision is fundamentally capped at int16's ±1 quant
// step, which is exactly the precision of the output anyway, so we
// throw away no useful precision.
//
// Phase tracked as a uint32_t fixed-point integer (2^32 = one cycle).
// Top 12 bits index the LUT; next 8 bits are the linear-interpolation
// fraction. With LUT entries up to ~16384 in magnitude, the max
// inter-entry delta is ~25 units near zero crossings — frac*delta/256
// stays well within int32 with no overflow risk.
//
// Worst-case interpolation error is bounded by AMP × (LUT_step)² / 8
// ≈ 16384 × (2π/4096)² / 8 ≈ 0.005 ADC counts — invisible in int16
// quantization, and ~−130 dB below full scale. Audio quality is
// indistinguishable from the direct sin() path.
#define MOD_LUT_BITS  12
#define MOD_LUT_SIZE  (1u << MOD_LUT_BITS)   // 4096
#define MOD_LUT_MASK  (MOD_LUT_SIZE - 1u)
// L7.14-fix5: place LUT in PSRAM. L7.14-fix4 put it in internal-RAM
// BSS (default), which consumed 8 KB of the largest contiguous block
// — that block was already razor-thin (~32 KB) and is exactly what
// js8sync's task stack needs. Result: js8sync OOM at boot, JS8 RX
// broken. PSRAM placement leaves internal RAM untouched; the 64 KB
// data cache easily holds the full 8 KB LUT after the first warm-up
// pass, so the inner-loop hot-read stays effectively L1-speed.
EXT_RAM_BSS_ATTR static int16_t s_sin_lut[MOD_LUT_SIZE];
static bool    s_sin_lut_ready = false;

// Build the int16 sine LUT in place. Idempotent — guarded by the
// ready flag so a second call is free. Called from modulator_init
// so the table is ready before any TX self-test or runtime render.
static void build_sin_lut(void) {
    if (s_sin_lut_ready) return;
    for (uint32_t k = 0; k < MOD_LUT_SIZE; ++k) {
        const double a = TWO_PI * (double)k / (double)MOD_LUT_SIZE;
        // sin(a) ∈ [-1, +1] → ×AMP → int16 cast. AMP=16384 is a
        // safe full-scale (peak |int16| = 32767), no overshoot risk.
        s_sin_lut[k] = (int16_t)(sin(a) * AMP);
    }
    s_sin_lut_ready = true;
}

// ── Public API ──────────────────────────────────────────────────────

esp_err_t nanojs8_js8_modulator_init(void)
{
    if (s_modulator_init_ok) {
        // Idempotent — common case is a re-call from self-test or
        // a future TX-screen init path. Don't realloc.
        return ESP_OK;
    }

    s_samples = (int16_t *)heap_caps_malloc(
        NANOJS8_JS8_MODULATE_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_samples) {
        ESP_LOGE(TAG, "PSRAM alloc failed: needed %u bytes for modulator buf",
                 (unsigned)NANOJS8_JS8_MODULATE_BUFFER_BYTES);
        return ESP_ERR_NO_MEM;
    }

    // Pre-zero the whole buffer once. The modulate path will overwrite
    // the data region but the start-delay portion stays zero forever
    // (it's silent by definition). This also means a TX immediately
    // after init transmits a clean 500 ms of silence followed by
    // whatever modulate produced.
    memset(s_samples, 0, NANOJS8_JS8_MODULATE_BUFFER_BYTES);

    // L7.14-fix4: build the sine LUT now so the first TX self-test
    // (and every runtime render after) hits the cached table. Cost
    // ~10–30 ms here vs ~2 s saved later on every render.
    build_sin_lut();

    s_modulator_init_ok = true;
    ESP_LOGI(TAG, "Modulator buffer allocated: %u bytes in PSRAM "
                  "(%u samples, %.2f s of 12 kHz audio)",
             (unsigned)NANOJS8_JS8_MODULATE_BUFFER_BYTES,
             (unsigned)NANOJS8_JS8_MODULATE_SAMPLE_COUNT,
             (double)NANOJS8_JS8_MODULATE_SAMPLE_COUNT
               / (double)NANOJS8_JS8_MODULATE_SAMPLE_RATE);
    return ESP_OK;
}

const int16_t *nanojs8_js8_modulator_get_samples(size_t *out_n)
{
    if (!s_modulator_init_ok) {
        if (out_n) *out_n = 0;
        return NULL;
    }
    if (out_n) *out_n = NANOJS8_JS8_MODULATE_SAMPLE_COUNT;
    return s_samples;
}

esp_err_t nanojs8_js8_modulator_load_samples(const int16_t *src, size_t n)
{
    // L7.11f-fix2: copy a previously-modulated frame's samples back
    // into the modulator's internal PSRAM buffer so the existing
    // render path (nanojs8_tx_audio_render_from_modulator) can pick
    // them up without any new render API.
    //
    // This is the "MicroJS8-style" pre-modulate-all path: the
    // multi-frame TX worker pre-modulates every frame into its own
    // PSRAM cache buffer up-front, then per slot calls load_samples()
    // to swap the right frame in before render+transmit. The
    // 308 KB memcpy runs in well under a millisecond on the S3 with
    // PSRAM at 80 MHz, so it's effectively free in the inter-slot
    // budget.
    if (!s_modulator_init_ok || !s_samples) {
        ESP_LOGE(TAG, "load_samples: modulator not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!src) {
        ESP_LOGE(TAG, "load_samples: NULL source");
        return ESP_ERR_INVALID_ARG;
    }
    if (n != NANOJS8_JS8_MODULATE_SAMPLE_COUNT) {
        ESP_LOGE(TAG, "load_samples: sample count %u != expected %d",
                 (unsigned)n,
                 (int)NANOJS8_JS8_MODULATE_SAMPLE_COUNT);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_samples, src, NANOJS8_JS8_MODULATE_BUFFER_BYTES);
    return ESP_OK;
}

// ── Shared render: 79 tones → CPFSK samples into PSRAM ──────────────
//
// Lifted out of modulate_heartbeat in L7.11e so modulate_text (and any
// future modulator entry points) can use the same render pass without
// duplicating math. The render itself is the dominant cost (~2 s for
// 151,680 sin() calls); the encode step before it is microseconds for
// the float work but variable for the regex inside pack().

static esp_err_t render_tones_to_buffer(const uint8_t tones[NANOJS8_JS8_NUM_TONES],
                                         double audio_freq_hz,
                                         const char *who)
{
    // Audio frequency sanity: tone 0 must be > 0 Hz, and tone 7 (highest
    // data tone, since Costas range is also [0,7]) must clear Nyquist
    // with some margin: audio_freq_hz + 43.75 Hz < SR/2. Practical
    // JS8Call range is roughly 500 - 2500 Hz.
    if (audio_freq_hz <= 0.0 ||
        audio_freq_hz + 7.0 * NANOJS8_JS8_MODULATE_TONE_SPACING_HZ
          >= (double)NANOJS8_JS8_MODULATE_SAMPLE_RATE / 2.0) {
        ESP_LOGE(TAG, "%s: audio_freq_hz=%.2f out of range",
                 who, audio_freq_hz);
        return ESP_FAIL;
    }

    // L7.14-fix4: CPFSK render via int16 sine LUT + fixed-point
    // phase accumulator. Replaces ~151,680 software-emulated double
    // sin() calls with a table lookup + linear interpolation, cutting
    // total render time from ~2 s to ~50 ms on the ESP32-S3 at 160 MHz.
    //
    // Phase is a uint32_t — its full range maps to one cycle (2π).
    // Wrap-around at the cycle boundary is automatic via the
    // overflow of unsigned addition; no explicit modulo needed.
    uint32_t phase_fp = 0;
    size_t   idx      = DATA_START;

    // Conversion factor from "frequency in Hz" to phase increment
    // per sample (in fixed-point units). Each cycle = 2^32, period
    // = 1/freq seconds, sample period = 1/SR seconds, so increments
    // per sample = freq/SR cycles = freq/SR × 2^32 fp units.
    const double FS_TO_FP_PER_HZ =
        4294967296.0 / (double)NANOJS8_JS8_MODULATE_SAMPLE_RATE;

    for (int sym = 0; sym < NSYM; ++sym) {
        const double freq =
            audio_freq_hz +
            (double)tones[sym] * NANOJS8_JS8_MODULATE_TONE_SPACING_HZ;
        const uint32_t dphi_fp = (uint32_t)(freq * FS_TO_FP_PER_HZ);

        for (int i = 0; i < SPS; ++i) {
            // Top 12 bits → LUT index; next 8 bits → interp fraction.
            const uint32_t idx_int  = phase_fp >> (32 - MOD_LUT_BITS);
            const uint32_t frac_int = (phase_fp >> (32 - MOD_LUT_BITS - 8))
                                      & 0xFFu;
            // LUT entries are pre-scaled to AMP, so s0/s1 are the
            // int16 output values directly. Interpolate in int32
            // to avoid intermediate overflow on the multiply.
            const int32_t s0    = (int32_t)s_sin_lut[idx_int];
            const int32_t s1    = (int32_t)s_sin_lut[(idx_int + 1u)
                                                     & MOD_LUT_MASK];
            const int32_t delta = s1 - s0;
            const int32_t sample = s0 + ((delta * (int32_t)frac_int) >> 8);
            s_samples[idx++] = (int16_t)sample;
            phase_fp += dphi_fp;
        }
    }

    // Defensive: idx should equal NANOJS8_JS8_MODULATE_SAMPLE_COUNT exactly.
    if (idx != NANOJS8_JS8_MODULATE_SAMPLE_COUNT) {
        ESP_LOGE(TAG, "%s: BUG — final idx=%u != expected %u",
                 who, (unsigned)idx,
                 (unsigned)NANOJS8_JS8_MODULATE_SAMPLE_COUNT);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t nanojs8_js8_modulate_heartbeat(const char *text,
                                          const char *callsign,
                                          double audio_freq_hz)
{
    if (!s_modulator_init_ok || !s_samples) {
        ESP_LOGE(TAG, "modulate_heartbeat: modulator not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Phase 1: encode → tones (heartbeat-specific path, lighter than pack).
    uint8_t tones[NANOJS8_JS8_NUM_TONES];
    if (!nanojs8_js8_encode_heartbeat(text, callsign, tones)) {
        ESP_LOGE(TAG, "modulate_heartbeat: encode_heartbeat returned false");
        return ESP_FAIL;
    }

    // Phase 2: shared CPFSK render.
    return render_tones_to_buffer(tones, audio_freq_hz, "modulate_heartbeat");
}

esp_err_t nanojs8_js8_modulate_text(const char *text,
                                     const char *mycall,
                                     const char *mygrid,
                                     double audio_freq_hz)
{
    // L7.11f: thin wrapper. Equivalent to modulate_text_frame with
    // frame_index=0 — preserves the single-frame call sites.
    return nanojs8_js8_modulate_text_frame(text, mycall, mygrid,
                                            0, audio_freq_hz);
}

esp_err_t nanojs8_js8_modulate_text_frame(const char *text,
                                           const char *mycall,
                                           const char *mygrid,
                                           size_t frame_index,
                                           double audio_freq_hz)
{
    if (!s_modulator_init_ok || !s_samples) {
        ESP_LOGE(TAG, "modulate_text_frame: modulator not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!text || !mycall || !mygrid) {
        ESP_LOGE(TAG, "modulate_text_frame: null arg");
        return ESP_ERR_INVALID_ARG;
    }

    // Phase 1: encode → tones (specific frame).
    uint8_t tones[NANOJS8_JS8_NUM_TONES];
    if (!nanojs8_js8_encode_text_frame(text, mycall, mygrid,
                                        frame_index, tones)) {
        ESP_LOGE(TAG, "modulate_text_frame: encode_text_frame failed "
                      "for '%s' frame %u",
                 text, (unsigned)frame_index);
        return ESP_FAIL;
    }

    // Phase 2: shared CPFSK render.
    return render_tones_to_buffer(tones, audio_freq_hz,
                                   "modulate_text_frame");
}

bool nanojs8_js8_modulate_self_test(void)
{
    ESP_LOGI(TAG, "Self-test C: starting (alloc + modulate + sanity)");

    // C.1: allocator. We expect this to succeed cleanly — if it doesn't,
    // we have a memory regression elsewhere.
    if (nanojs8_js8_modulator_init() != ESP_OK) {
        ESP_LOGE(TAG, "Self-test C: modulator_init FAILED");
        return false;
    }

    // C.2: modulate the canonical heartbeat at a standard JS8Call audio
    // frequency. 1500 Hz is a common centre for the JS8Call sub-band;
    // tone 0 lands there, tone 7 at 1500 + 43.75 = 1543.75 Hz.
    const double test_freq_hz = 1500.0;
    if (nanojs8_js8_modulate_heartbeat("HB EN83", "W5DMH",
                                        test_freq_hz) != ESP_OK) {
        ESP_LOGE(TAG, "Self-test C: modulate_heartbeat FAILED");
        return false;
    }

    size_t n_samples = 0;
    const int16_t *samples = nanojs8_js8_modulator_get_samples(&n_samples);
    if (!samples || n_samples != NANOJS8_JS8_MODULATE_SAMPLE_COUNT) {
        ESP_LOGE(TAG, "Self-test C: get_samples returned %p / n=%u",
                 (void *)samples, (unsigned)n_samples);
        return false;
    }

    // C.3: start-delay region must be silent (we zero'd it at init and
    // never write to it). A non-zero sample here would indicate either
    // a buffer-overrun from the previous self-test or a bug in idx
    // initialization.
    for (size_t i = 0; i < DELAY_SAMPLES; ++i) {
        if (samples[i] != 0) {
            ESP_LOGE(TAG, "Self-test C: delay region not silent at i=%u "
                          "(s=%d)",
                     (unsigned)i, (int)samples[i]);
            return false;
        }
    }

    // C.4: data region statistics.
    //   Peak |x| should be very close to AMP = 16384. Allow ±2 for
    //   int16 quantization at the sin() peaks (peak slightly under in
    //   practice because we don't always land exactly on phase=π/2).
    //   Mean |x| for a sine wave is 2/π × peak ≈ 0.6366 × 16384 = 10430.
    //   For 8-FSK with continuous phase, mean |x| is the same — the
    //   instantaneous frequency varies but instantaneous amplitude
    //   doesn't.
    int32_t peak    = 0;
    int64_t sum_abs = 0;
    for (size_t i = DATA_START; i < NANOJS8_JS8_MODULATE_SAMPLE_COUNT; ++i) {
        int32_t s = (int32_t)samples[i];
        if (s < 0) s = -s;
        if (s > peak) peak = s;
        sum_abs += s;
    }
    const int32_t mean_abs =
        (int32_t)(sum_abs / (int64_t)(NANOJS8_JS8_MODULATE_SAMPLE_COUNT
                                      - DATA_START));

    // Peak window: AMP-2 ≤ peak ≤ AMP+0 (we cast to int16, no overshoot).
    if (peak < 16380 || peak > 16384) {
        ESP_LOGE(TAG, "Self-test C: peak=%d outside [16380, 16384]",
                 (int)peak);
        return false;
    }

    // Mean |x| window: theoretical 10430, allow ±400 (3.8%) for
    // quantization, phase-bin averaging at finite sample count.
    if (mean_abs < 10000 || mean_abs > 10800) {
        ESP_LOGE(TAG, "Self-test C: mean_abs=%d outside [10000, 10800]",
                 (int)mean_abs);
        return false;
    }

    ESP_LOGI(TAG, "Self-test C: PASS — %u samples, peak=%d, mean_abs=%d "
                  "(%.2f s of CPFSK at %.1f Hz base)",
             (unsigned)NANOJS8_JS8_MODULATE_SAMPLE_COUNT,
             (int)peak, (int)mean_abs,
             (double)NANOJS8_JS8_MODULATE_SAMPLE_COUNT
               / (double)NANOJS8_JS8_MODULATE_SAMPLE_RATE,
             test_freq_hz);
    return true;
}
