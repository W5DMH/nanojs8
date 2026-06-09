/*
 * llr_extract.c — JS8 LLR extraction from waterfall (L7.6)
 * =============================================================================
 * Given a sync candidate from ft8_lib's ftx_find_candidates and the
 * waterfall it was found in, produce 174 log-likelihood ratios suitable
 * for feeding into bpdecode174.
 *
 * STRUCTURALLY similar to ft8_lib's ft8_extract_likelihood / ft8_extract_symbol
 * (we use the same frame layout — 79 symbols, Costas at 0/36/72, 58 data
 * symbols at positions 7-35 and 43-71). The KEY DIFFERENCE is that JS8
 * uses NATURAL-BINARY tone-to-bit mapping, while FT8 uses GRAY CODE.
 *
 * Verified against:
 *   - gfsk8-modem-clean/src/whitening.h (the gfsk8 LLR formulas — they
 *     index ps[] directly without any Gray permutation)
 *   - gfsk8-modem-clean/src/api.cpp modulate() (the encoder writes tone
 *     value = bit triplet directly, no Gray map applied)
 *
 * Both LLR convention and bit ordering inside a symbol match between
 * our extractor and gfsk8's bpdecode174: positive LLR = bit is 1; bit
 * order within a triplet is MSB-first (logl[0] = MSB).
 *
 * License: GPL-3.0
 */

#include "js8_codec.h"

#include <math.h>
#include <stddef.h>

// Pull frame-layout constants from ft8_lib. JS8's frame timing is identical
// to FT8's: 79 symbols total, 21 sync (3×7 Costas), 58 data.
#include "constants.h"   // FT8_NN, FT8_ND, FT8_LENGTH_SYNC

// max() helper — 4-way for the LLR formulas below.
static inline float max4(float a, float b, float c, float d)
{
    float ab = (a > b) ? a : b;
    float cd = (c > d) ? c : d;
    return (ab > cd) ? ab : cd;
}

// ── Extract 3 LLRs from one symbol's 8 magnitude bins ────────────────────────
//
// `wf` points at the 8 magnitude bins for one symbol position in the
// waterfall. logl receives 3 LLRs (MSB first).
//
// Tone-to-bit mapping (NATURAL BINARY, no Gray):
//   tone 0 = 000   tone 4 = 100
//   tone 1 = 001   tone 5 = 101
//   tone 2 = 010   tone 6 = 110
//   tone 3 = 011   tone 7 = 111
//
// For each bit position b, the LLR is:
//   max(magnitudes where bit b is 1) - max(magnitudes where bit b is 0)
//
// LLR convention: positive value = bit is 1 (matches bpdecode174).
static void js8_extract_symbol(const WF_ELEM_T *wf, float *logl)
{
    // Decode WF_ELEM_T → float magnitude in dB.
    // (WF_ELEM_MAG is a macro defined in ft8_lib decode.h: uint8 → dB)
    const float s2_0 = WF_ELEM_MAG(wf[0]);
    const float s2_1 = WF_ELEM_MAG(wf[1]);
    const float s2_2 = WF_ELEM_MAG(wf[2]);
    const float s2_3 = WF_ELEM_MAG(wf[3]);
    const float s2_4 = WF_ELEM_MAG(wf[4]);
    const float s2_5 = WF_ELEM_MAG(wf[5]);
    const float s2_6 = WF_ELEM_MAG(wf[6]);
    const float s2_7 = WF_ELEM_MAG(wf[7]);

    // bit 2 (MSB): set in tones 4,5,6,7 ; clear in 0,1,2,3
    logl[0] = max4(s2_4, s2_5, s2_6, s2_7) - max4(s2_0, s2_1, s2_2, s2_3);

    // bit 1: set in tones 2,3,6,7 ; clear in 0,1,4,5
    logl[1] = max4(s2_2, s2_3, s2_6, s2_7) - max4(s2_0, s2_1, s2_4, s2_5);

    // bit 0 (LSB): set in tones 1,3,5,7 ; clear in 0,2,4,6
    logl[2] = max4(s2_1, s2_3, s2_5, s2_7) - max4(s2_0, s2_2, s2_4, s2_6);
}

// ── Locate a candidate's first-symbol magnitude pointer in the waterfall ─────
//
// Same offset arithmetic as ft8_lib's get_cand_mag (decode.c:20-27).
static const WF_ELEM_T *
js8_get_cand_mag(const ftx_waterfall_t *wf, const ftx_candidate_t *c)
{
    int offset = c->time_offset;
    offset = (offset * wf->time_osr) + c->time_sub;
    offset = (offset * wf->freq_osr) + c->freq_sub;
    offset = (offset * wf->num_bins) + c->freq_offset;
    return wf->mag + offset;
}

// ── LLR normalisation ────────────────────────────────────────────────────────
//
// Adapted from ft8_lib's ftx_normalize_logl (static in decode.c). Scales
// the 174 LLRs to a target variance so the BP decoder operates in a stable
// numerical range. Without this, raw dB-magnitude differences (range
// approximately -120..+120) drive tanh() to saturation immediately and the
// decoder converges poorly.
static void js8_normalize_llrs(float *llr174)
{
    float sum = 0.0f, sum2 = 0.0f;
    for (int i = 0; i < NANOJS8_JS8_LDPC_N; ++i) {
        sum  += llr174[i];
        sum2 += llr174[i] * llr174[i];
    }
    const float inv_n = 1.0f / (float)NANOJS8_JS8_LDPC_N;
    float variance = (sum2 - (sum * sum * inv_n)) * inv_n;

    // Guard against zero/negative variance (all-zero LLRs → no information).
    if (variance < 1e-20f) {
        for (int i = 0; i < NANOJS8_JS8_LDPC_N; ++i) llr174[i] = 0.0f;
        return;
    }

    // Empirically chosen scale factor 24.0 from ft8_lib (matches WSJT-X).
    const float norm = sqrtf(24.0f / variance);
    for (int i = 0; i < NANOJS8_JS8_LDPC_N; ++i) {
        llr174[i] *= norm;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public C API — single entry point
// ─────────────────────────────────────────────────────────────────────────────

void nanojs8_js8_extract_llrs(const ftx_waterfall_t *wf,
                               const ftx_candidate_t *cand,
                               float *llr_out)
{
    if (!wf || !cand || !llr_out) return;

    const WF_ELEM_T *mag = js8_get_cand_mag(wf, cand);

    // Walk the 58 data symbols, skipping Costas blocks at positions
    // 0-6 / 36-42 / 72-78. For k < 29 we offset by 7 (first Costas);
    // for k >= 29 we offset by 14 (first + middle Costas).
    //
    // Layout:
    //   k=0  → sym_idx=7      first data symbol
    //   k=28 → sym_idx=35     last symbol of first data block
    //   k=29 → sym_idx=43     first symbol of second data block
    //   k=57 → sym_idx=71     last data symbol
    for (int k = 0; k < FT8_ND; ++k) {
        const int sym_idx = k + ((k < 29) ? 7 : 14);
        const int bit_idx = 3 * k;
        const int block_abs = cand->time_offset + sym_idx;

        if (block_abs < 0 || block_abs >= wf->num_blocks) {
            // Out of waterfall window — emit zero LLR (no information).
            llr_out[bit_idx + 0] = 0.0f;
            llr_out[bit_idx + 1] = 0.0f;
            llr_out[bit_idx + 2] = 0.0f;
        } else {
            js8_extract_symbol(mag + (sym_idx * wf->block_stride),
                               llr_out + bit_idx);
        }
    }

    js8_normalize_llrs(llr_out);
}
