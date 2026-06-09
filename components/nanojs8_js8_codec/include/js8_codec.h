/*
 * js8_codec.h — JS8 LLR extraction + LDPC decode + CRC + message extract (L7.6)
 * =============================================================================
 * The complete RX pipeline AFTER sync detection. Takes a candidate (from
 * ft8_lib's ftx_find_candidates) and a waterfall, runs it through:
 *
 *   1. LLR extraction (natural-binary, no Gray code — JS8-specific)
 *   2. LLR normalization to ~unit variance
 *   3. Belief-propagation LDPC decode (lifted from gfsk8)
 *   4. CRC-12 verification (lifted from gfsk8, with the XOR-42 quirk)
 *   5. 12-character JS8 alphabet extraction
 *
 * If all steps succeed, returns the 12-character raw message. This is not
 * human-readable text yet — JS8's actual text is Varicode + JSC compressed
 * INSIDE these 12 characters (or across multiple frames for long messages).
 * Layer 7.7 will add the Varicode + JSC unpack.
 *
 * Frame-type bits and remaining bits-after-message-words are exposed via
 * out_frame_type for the higher-layer protocol decoder.
 *
 * License: GPL-3.0
 *   - bpdecode174, Mn, Nm, checkCRC12, extractmessage174, alphabet, CRC12()
 *     wrapper all lifted from gfsk8-modem-clean (GPL-3.0). See ATTRIBUTION.md.
 *   - LLR extraction is our own implementation (modelled on ft8_lib's
 *     ft8_extract_likelihood but with the Gray-code permutation removed, as
 *     JS8 uses natural-binary tone mapping).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>     // size_t (for L7.11f multi-frame APIs)
#include "esp_err.h"

// We need ftx_waterfall_t + ftx_candidate_t from ft8_lib's decode.h. Pull
// them via the public ft8_lib header. (REQUIRES nanojs8_ft8_lib in our
// component CMakeLists.txt makes this available.)
#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Number of bits in a JS8 codeword (174) — for fixed-size LLR buffers.
#define NANOJS8_JS8_LDPC_N        174

/// Number of info+CRC bits in a JS8 message (87). 75 message + 12 CRC.
#define NANOJS8_JS8_LDPC_K        87

/// Number of characters in the human-readable JS8 message frame.
#define NANOJS8_JS8_MSG_CHARS     12

/// Maximum LDPC BP iterations attempted per decode (matches gfsk8 default).
#define NANOJS8_JS8_MAX_ITERS     30

/**
 * Result of a single decode attempt.
 */
typedef struct {
    bool     crc_ok;        // CRC-12 passed
    int      ldpc_iters;    // BP iterations used
    int      ldpc_errors;   // residual parity-check failures
    uint8_t  frame_type;    // 3-bit JS8 frame type (bits 75..77)
    char     message[NANOJS8_JS8_MSG_CHARS + 1]; // NUL-terminated 12-char raw
} nanojs8_js8_decode_result_t;

/**
 * Extract per-bit log-likelihoods from a sync candidate's position in the
 * waterfall. Uses natural-binary 8-FSK demap (no Gray code), then normalises
 * to ~unit variance.
 *
 * The output llr174 follows the convention: positive LLR = bit is 1.
 *
 * @param wf       waterfall produced by ft8_lib's monitor
 * @param cand     candidate from ftx_find_candidates
 * @param llr_out  output: NANOJS8_JS8_LDPC_N (174) floats
 */
void nanojs8_js8_extract_llrs(const ftx_waterfall_t *wf,
                               const ftx_candidate_t *cand,
                               float *llr_out);

/**
 * Run the full LDPC + CRC + message-extract pipeline on a 174-LLR codeword.
 *
 * @param llr174 174 input log-likelihoods (positive = bit is 1)
 * @param result populated with the outcome; result->crc_ok indicates
 *               whether the decode produced a valid message.
 * @return       true if CRC passed (message is valid), false otherwise
 *               (the LDPC may have decoded but CRC failed, or LDPC
 *               failed to converge entirely).
 */
bool nanojs8_js8_decode_llrs(const float *llr174,
                              nanojs8_js8_decode_result_t *result);

// ─── L7.7: protocol-layer message unpack ─────────────────────────────────────

/// JS8 submode IDs — match Varicode::SubmodeType / gfsk8::Submode.
#define NANOJS8_JS8_SUBMODE_NORMAL  0
#define NANOJS8_JS8_SUBMODE_FAST    1
#define NANOJS8_JS8_SUBMODE_TURBO   2
#define NANOJS8_JS8_SUBMODE_SLOW    4
#define NANOJS8_JS8_SUBMODE_ULTRA   8

/// FrameType sub-classification returned in nanojs8_js8_message_t::frame_subtype.
/// Matches Varicode::FrameType enum values.
#define NANOJS8_JS8_FRAME_UNKNOWN           255
#define NANOJS8_JS8_FRAME_HEARTBEAT         0    // [000]
#define NANOJS8_JS8_FRAME_COMPOUND          1    // [001]
#define NANOJS8_JS8_FRAME_COMPOUND_DIRECTED 2    // [010]
#define NANOJS8_JS8_FRAME_DIRECTED          3    // [011]
#define NANOJS8_JS8_FRAME_DATA              4    // [10X]
#define NANOJS8_JS8_FRAME_DATA_COMPRESSED   6    // [11X]

/// TransmissionType flag bits — match Varicode::TransmissionType.
#define NANOJS8_JS8_TX_NORMAL   0   // 000 middle frame
#define NANOJS8_JS8_TX_FIRST    1   // 001 first frame of multi-frame msg
#define NANOJS8_JS8_TX_LAST     2   // 010 last frame of multi-frame msg
#define NANOJS8_JS8_TX_DATA     4   // 100 raw data frame

/// Maximum human-readable message length. Single-frame JS8 messages
/// rarely exceed ~80 chars; 256 is comfortable headroom.
#define NANOJS8_JS8_MSG_TEXT_MAX  256

/// JS8 callsigns are at most 11 chars (e.g. "AA1AA/QRP", "K1ABC/PORTABLE"
/// edge cases). 16 is comfortable headroom including NUL.
#define NANOJS8_JS8_CALL_MAX   16
/// Maidenhead grid: 4 or 6 chars + NUL.
#define NANOJS8_JS8_GRID_MAX   8
/// JS8 verbs: "HEARTBEAT", "SNR", "ACK", "MSG", "@HB HEARTBEAT", etc.
#define NANOJS8_JS8_VERB_MAX   24
/// Verb argument / numeric body: "-20", "+16", "EM44XX", etc.
#define NANOJS8_JS8_BODY_MAX   32

/**
 * Parsed protocol-layer view of a successfully-decoded JS8 frame.
 *
 * `text` is the human-readable rendering (what the operator sees in
 * a chat-style log). The structured fields (`from_call`, `to_call`,
 * `grid`, `verb`, `body`) carry the same info pre-parsed for callers
 * that want to populate a tabular UI (e.g. HEARD list keyed by call,
 * DIRECTED log keyed by verb).
 *
 * Unused structured fields are NUL-terminated empty strings — never
 * uninitialised — so callers can treat them uniformly.
 */
typedef struct {
    bool    ok;             // true if a strategy matched the frame
    uint8_t frame_subtype;  // NANOJS8_JS8_FRAME_*; UNKNOWN (255) if !ok
    bool    is_heartbeat;
    bool    is_directed;
    char    text[NANOJS8_JS8_MSG_TEXT_MAX];   // NUL-terminated

    // ─── L7.9: structured fields for the activity store / UI ─────────
    char    from_call[NANOJS8_JS8_CALL_MAX];  // sender; "" if unknown
    char    to_call[NANOJS8_JS8_CALL_MAX];    // recipient; "" if not directed
    char    grid[NANOJS8_JS8_GRID_MAX];       // 4-6 char grid if heartbeat; "" else
    char    verb[NANOJS8_JS8_VERB_MAX];       // "HEARTBEAT","SNR","ACK","MSG",...
    char    body[NANOJS8_JS8_BODY_MAX];       // SNR num, grid arg, etc.
} nanojs8_js8_message_t;

/**
 * Unpack a CRC-validated 12-character frame into human-readable text.
 *
 * Tries the JS8 strategy chain (FastData → Data → Heartbeat → Compound →
 * Directed) in the same order as upstream DecodedText. The first strategy
 * that produces a non-empty result wins.
 *
 * On success, `out->ok` is true, `out->text` holds a NUL-terminated string,
 * and `out->frame_subtype` is one of NANOJS8_JS8_FRAME_*.
 *
 * On failure (no strategy matched), `out->ok` is false and `out->text`
 * is empty. The caller can fall back to displaying the raw 12 chars.
 *
 * @param raw12       12-character alphabet-encoded frame (from
 *                    nanojs8_js8_decode_result_t::message)
 * @param frame_type  3-bit TransmissionType from
 *                    nanojs8_js8_decode_result_t::frame_type
 * @param submode     NANOJS8_JS8_SUBMODE_* (only Normal = 0 is currently
 *                    in use; reserved for future submode support)
 * @param out         filled with the parsed result
 * @return            true on successful unpack
 */
bool nanojs8_js8_unpack_message(const char *raw12,
                                 uint8_t frame_type,
                                 int submode,
                                 nanojs8_js8_message_t *out);

// ─── L7.8: multi-frame message assembly ──────────────────────────────────────

/// Maximum number of in-flight (concurrent) multi-frame assemblies the
/// assembler can track. 8 is generous on a single HF band — a busy 7.078
/// stream rarely has more than 4-5 active QSOs at once.
#define NANOJS8_JS8_ASM_MAX_SLOTS         8

/// Frequency match tolerance when looking up a slot by audio frequency.
/// Real signals drift a few Hz between frames; ±25 Hz is reliable without
/// merging neighbouring stations.
#define NANOJS8_JS8_ASM_FREQ_TOL_HZ       25.0f

/// Idle timeout — buffers older than this without an update are discarded.
/// Set to 6 JS8 Normal slots (90 s) — long enough to cover an interrupted
/// burst, short enough that a stale buffer doesn't merge into the next QSO.
#define NANOJS8_JS8_ASM_TIMEOUT_MS        (90u * 1000u)

/// Maximum assembled message length. JS8 multi-frame messages can be long
/// (200+ chars) — 512 covers any realistic exchange.
#define NANOJS8_JS8_ASM_TEXT_MAX          512

/// Assembly origin classification, exposed via nanojs8_js8_assembly_t.kind.
typedef enum {
    NANOJS8_JS8_ASM_KIND_NONE = 0,        // no output (still in progress)
    NANOJS8_JS8_ASM_KIND_SINGLE_FRAME,    // FIRST+LAST bits both set
    NANOJS8_JS8_ASM_KIND_COMPLETE,        // saw FIRST → ... → LAST sequence
    NANOJS8_JS8_ASM_KIND_PARTIAL_TAIL,    // saw LAST but never saw FIRST (joined mid-stream)
} nanojs8_js8_asm_kind_t;

/**
 * Output of one assembly step. If `kind != NONE`, a complete (or
 * partial-tail) message is in `text` ready to display.
 */
typedef struct {
    nanojs8_js8_asm_kind_t kind;
    float                  freq_hz;       // assembled at this frequency
    uint16_t               frame_count;   // frames that contributed
    char                   text[NANOJS8_JS8_ASM_TEXT_MAX];
} nanojs8_js8_assembly_t;

/**
 * Feed one successfully-decoded frame to the assembler.
 *
 * Only DATA-class frames (frame_subtype = NANOJS8_JS8_FRAME_DATA or
 * NANOJS8_JS8_FRAME_DATA_COMPRESSED) take part in multi-frame assembly.
 * Other frame types (heartbeats, single-frame directed messages) are
 * passed through with kind = SINGLE_FRAME.
 *
 * The TransmissionType bits in `tx_type` select the assembler action:
 *   - FIRST | LAST set together → emit single-frame immediately
 *   - FIRST set             → start a new buffer at this frequency
 *   - LAST set              → append + emit + clear buffer
 *   - neither set (middle)  → append (or seed) buffer
 *
 * @param freq_hz        audio centre frequency where the frame was decoded
 * @param tx_type        3-bit TransmissionType (NANOJS8_JS8_TX_* flags)
 * @param frame_subtype  Varicode FrameType (NANOJS8_JS8_FRAME_*)
 * @param frame_text     parsed text from nanojs8_js8_unpack_message
 * @param now_ms         millisecond timestamp for timeout bookkeeping
 *                       (any monotonic source — esp_timer_get_time / 1000)
 * @param out            populated when kind != NONE
 * @return               true if `out` contains a message to display
 */
bool nanojs8_js8_assemble_frame(float freq_hz,
                                 uint8_t tx_type,
                                 uint8_t frame_subtype,
                                 const char *frame_text,
                                 uint32_t now_ms,
                                 nanojs8_js8_assembly_t *out);

/// Clear all in-flight assemblies. Call on UTC reset or band change.
void nanojs8_js8_assembler_reset(void);

/// Get number of in-flight (occupied) assembly slots for diagnostics.
uint32_t nanojs8_js8_assembler_active_count(void);

// ─── L7.11a: TX path — encoder (heartbeat verbs) ─────────────────────────────

/// Number of physical-layer tones in a JS8 Normal frame. Exposed as a
/// compile-time constant so callers can size their output buffer (e.g.
/// `uint8_t tones[NANOJS8_JS8_NUM_TONES];`) without a runtime alloc.
#define NANOJS8_JS8_NUM_TONES   79

/**
 * Encode a heartbeat-style message into 79 JS8 Normal 8-FSK tone values.
 *
 * Supported `text` inputs (per the JS8Call heartbeat regex):
 *   "HB", "HB EN83", "HEARTBEAT", "HEARTBEAT EM44",
 *   "CQ", "CQ EN83", "CQ CQ CQ EN83", "CQ DX", "CQ QRP EM73",
 *   "CQ CONTEST", "CQ FIELD", "CQ FD"
 *
 * Optional grid (the last token of `text`) must be a 4-character standard
 * Maidenhead locator (e.g. "EN83"). Invalid or missing grids encode as the
 * "no grid" sentinel — the message is still legal on air.
 *
 * `callsign` is the operator's transmitting call (typically `cfg->callsign`).
 * Empty callsign causes the encode to fail.
 *
 * On success, `out_tones` is filled with NANOJS8_JS8_NUM_TONES values, each
 * in the range [0, 7]. Sync tones (Costas array {4,2,5,6,1,3,0}) appear at
 * positions 0–6, 36–42, and 72–78; the remaining 58 positions are data.
 *
 * No memory allocation persists beyond the call (intermediate std::vector
 * is on the stack).
 *
 * @param text     human-readable heartbeat verb + optional grid
 * @param callsign operator's transmitting callsign (NUL-terminated)
 * @param out_tones output: 79 bytes of tone values in [0,7]
 * @return         true on success, false if `text` doesn't match the
 *                 heartbeat regex / `callsign` is empty / encode failed.
 */
bool nanojs8_js8_encode_heartbeat(const char *text,
                                   const char *callsign,
                                   uint8_t out_tones[NANOJS8_JS8_NUM_TONES]);

/**
 * L7.11e: Generic JS8 message encoder. Wraps gfsk8::pack() — which
 * auto-classifies the input as heartbeat, directed, CQ, free-text,
 * etc. — and then runs gfsk8::encode() on the first packed frame to
 * produce 79 tones.
 *
 * Multi-frame messages (long bodies that produce more than one
 * TxFrame) are NOT yet supported — only the first frame is encoded.
 * The function logs a warning but still returns true so single-frame
 * traffic isn't blocked by long-text inputs. Multi-frame TX (queued
 * back-to-back across consecutive slots) will land in a later layer.
 *
 * Use this in preference to nanojs8_js8_encode_heartbeat() for any
 * non-heartbeat traffic. Both functions exist in parallel because
 * (a) the boot self-test depends on encode_heartbeat's exact
 * behavior and we don't want to perturb its verification, and (b)
 * heartbeats are a hot path where we'd rather not pay pack()'s
 * regex overhead. Wire forms produced by gfsk8::pack include:
 *
 *   "@HB HEARTBEAT EN83"      → heartbeat (type 6/0)
 *   "CQ CQ CQ EN83"           → CQ (type 0)
 *   "@ALLCALL QUERY MSGS"     → directed @ALLCALL (type 3)
 *   "K1ABC SNR?"              → directed verb (type 3)
 *   "K1ABC hello there"       → directed free-text (type 0/3)
 *
 * Stack: this path uses std::regex inside Varicode::buildMessageFrames
 * plus the LDPC encode chain — must be called from a task with at
 * least 8 KB of stack. main_task (~3.5 KB) is too small.
 *
 * @param text     wire-form text to transmit (no "<from>: " prefix —
 *                 the encoder adds it automatically)
 * @param mycall   operator's transmitting callsign (NUL-terminated)
 * @param mygrid   operator's grid square (NUL-terminated; some frame
 *                 types use this — pass even if `text` already
 *                 contains a grid)
 * @param out_tones output: 79 bytes of tone values in [0,7]
 * @return         true on success, false if pack/encode failed.
 */
bool nanojs8_js8_encode_text(const char *text,
                              const char *mycall,
                              const char *mygrid,
                              uint8_t out_tones[NANOJS8_JS8_NUM_TONES]);

/**
 * L7.11f multi-frame TX support: report how many physical-layer JS8
 * frames the given wire text will encode to.
 *
 * Most short messages (heartbeats, CQ, simple verbs like SNR?) fit in
 * one frame. Longer bodies — free-text, MSG with body — may require
 * 2-4 frames, each occupying one 15 s JS8 slot.
 *
 * Use this before transmitting to:
 *   - Decide whether to plan a multi-slot TX sequence
 *   - Warn the operator if their message is unexpectedly long
 *
 * Stack: pack() inside uses std::regex; needs >= 8 KB stack. Same
 * constraint as encode_text/modulate_text.
 *
 * @param text   wire-form text
 * @param mycall operator's callsign
 * @param mygrid operator's grid
 * @return       number of frames (1..N), or 0 on error / empty / null input.
 */
size_t nanojs8_js8_text_frame_count(const char *text,
                                     const char *mycall,
                                     const char *mygrid);

/**
 * L7.11f multi-frame TX support: encode a specific frame from a wire
 * text that may produce multiple frames. nanojs8_js8_encode_text() is
 * equivalent to this with frame_index=0.
 *
 * Callers should first call nanojs8_js8_text_frame_count() to learn
 * the total, then iterate frame_index = 0..N-1 for full transmission.
 * Each frame is encoded in a fresh pack() call (stateless API).
 *
 * @param text         wire-form text
 * @param mycall       operator's callsign
 * @param mygrid       operator's grid
 * @param frame_index  0-indexed frame to encode (must be < frame_count)
 * @param out_tones    output 79-byte tone array
 * @return             true on success, false if frame_index out of
 *                     range or encode failed.
 */
bool nanojs8_js8_encode_text_frame(const char *text,
                                    const char *mycall,
                                    const char *mygrid,
                                    size_t frame_index,
                                    uint8_t out_tones[NANOJS8_JS8_NUM_TONES]);

/**
 * Boot-time self-test of the TX encode chain. Runs:
 *
 *   1. Varicode pack/unpack round-trip on a representative heartbeat
 *      ("HB EN83" from a known callsign), verifying the protocol layer
 *      preserves all fields (verb type, grid, callsign).
 *   2. Full encode → 79 tones, verifying Costas {4,2,5,6,1,3,0} appears
 *      at positions 0–6, 36–42, and 72–78, and that every tone is in [0,7].
 *   3. (L7.11b) Modulator allocation + sample generation → 157,680 int16
 *      samples in PSRAM. Verifies the start-delay region is silent, peak
 *      amplitude is in the expected range, and mean |x| matches the
 *      analytical CPFSK envelope (≈ 2/π × amplitude).
 *
 * Logs result via ESP_LOGI / ESP_LOGE. Returns true if all subtests pass.
 *
 * Called from main.c after subsystem init so any failure surfaces in
 * the boot log before the operator ever tries to transmit.
 */
bool nanojs8_js8_encode_self_test(void);

// ─── L7.11b: TX path — modulator (tones → 12 kHz CPFSK PCM samples) ──────────

/// Sample rate of the modulator output (Hz). Matches gfsk8's RX path,
/// so RX-side self-decoding would work without resampling.
#define NANOJS8_JS8_MODULATE_SAMPLE_RATE   12000

/// Tone spacing in Hz for JS8 Normal (= SR / samples-per-symbol).
/// 12000 / 1920 = 6.25 Hz exactly. Used by both modulator and any
/// caller that needs to label the audio frequency of a given tone.
#define NANOJS8_JS8_MODULATE_TONE_SPACING_HZ  6.25

/// Total samples produced by a JS8 Normal modulation pass:
///   500 ms start delay (6000 samples) + 79 × 1920 = 157,680 samples
/// → 13.14 s of audio, comfortably inside the 15 s slot.
#define NANOJS8_JS8_MODULATE_SAMPLE_COUNT  157680

/// Buffer footprint in bytes (int16 PCM, mono). 315,360 ≈ 308 KB — must
/// live in PSRAM; would never fit in the ~180 KB of free DRAM.
#define NANOJS8_JS8_MODULATE_BUFFER_BYTES \
        (NANOJS8_JS8_MODULATE_SAMPLE_COUNT * 2)

/**
 * Allocate the modulator's PSRAM sample buffer. Idempotent — second
 * call returns ESP_OK without doing anything. MUST succeed before
 * nanojs8_js8_modulate_heartbeat() can be called.
 *
 * Returns ESP_ERR_NO_MEM if PSRAM allocation fails (the buffer is
 * ~308 KB; we have ~6 MB free PSRAM, so failure indicates fragmentation
 * or a different memory regression — log will show details).
 *
 * The allocation is permanent (held until reboot) by design: we trade
 * 308 KB of PSRAM (out of 8 MB) for a TX path that never calls malloc/
 * free during operation. Slot-aligned timing in L7.11d requires this.
 */
esp_err_t nanojs8_js8_modulator_init(void);

/**
 * Encode + modulate a heartbeat-style message into the modulator's
 * internal PSRAM buffer. Equivalent to nanojs8_js8_encode_heartbeat()
 * followed by a CPFSK-render pass into PCM at NANOJS8_JS8_MODULATE_SAMPLE_RATE.
 *
 * @param text          heartbeat verb + optional grid (see encode_heartbeat)
 * @param callsign      operator's transmitting callsign
 * @param audio_freq_hz audio-frequency offset of tone 0 (typ. 1000-2000 Hz);
 *                      tone i lands at audio_freq_hz + i × 6.25 Hz.
 * @return  ESP_OK on success, ESP_ERR_INVALID_STATE if modulator not init,
 *          ESP_FAIL if encode failed or audio_freq_hz is out of sensible
 *          range (≤ 0 Hz or ≥ SAMPLE_RATE/2 - tone-band-width).
 *
 * After ESP_OK, retrieve the samples via nanojs8_js8_modulator_get_samples().
 */
esp_err_t nanojs8_js8_modulate_heartbeat(const char *text,
                                          const char *callsign,
                                          double audio_freq_hz);

/**
 * L7.11e: Modulate an arbitrary JS8 wire-form message into the
 * internal PSRAM buffer. Uses nanojs8_js8_encode_text() to produce
 * 79 tones and then runs the same CPFSK render as modulate_heartbeat.
 *
 * Same stack-usage caveat: must be called from a task with at least
 * 8 KB stack because the pack() call uses std::regex.
 *
 * @param text          wire-form text (e.g. "CQ CQ CQ EN83",
 *                      "@ALLCALL QUERY MSGS", "K1ABC hello")
 * @param mycall        operator's transmitting callsign
 * @param mygrid        operator's grid square
 * @param audio_freq_hz audio-frequency offset of tone 0 (typ. 1500 Hz)
 * @return  ESP_OK on success, ESP_ERR_INVALID_STATE if modulator not
 *          init, ESP_FAIL if encode failed or audio_freq_hz out of range.
 *
 * After ESP_OK, retrieve the samples via nanojs8_js8_modulator_get_samples().
 */
esp_err_t nanojs8_js8_modulate_text(const char *text,
                                     const char *mycall,
                                     const char *mygrid,
                                     double audio_freq_hz);

/**
 * L7.11f multi-frame modulator: encode a specific frame from a wire
 * text into the PSRAM buffer. modulate_text() is equivalent to this
 * with frame_index=0.
 *
 * The buffer holds ONE frame at a time. Multi-frame TX is achieved by
 * calling modulate_text_frame(i) → render → transmit → repeat for
 * frame_index = 0..N-1 across consecutive JS8 slots.
 *
 * Same 8 KB stack requirement as modulate_text.
 */
esp_err_t nanojs8_js8_modulate_text_frame(const char *text,
                                           const char *mycall,
                                           const char *mygrid,
                                           size_t frame_index,
                                           double audio_freq_hz);

/**
 * Get a const pointer to the modulator's PSRAM buffer plus its sample
 * count. Returns NULL if the modulator hasn't been initialized.
 * `out_n` is optional; pass NULL if you only need the pointer.
 */
const int16_t *nanojs8_js8_modulator_get_samples(size_t *out_n);

/**
 * L7.11f-fix2: copy a previously-modulated frame's samples back into
 * the modulator's PSRAM buffer. Used by the multi-frame TX worker's
 * "pre-modulate all" path (MicroJS8-style): the worker pre-modulates
 * each frame into its own PSRAM cache buffer up front, then per slot
 * calls this to swap the right frame into the modulator before the
 * usual nanojs8_tx_audio_render_from_modulator() pass.
 *
 * No CPFSK work happens here — it's a single PSRAM-to-PSRAM memcpy of
 * NANOJS8_JS8_MODULATE_BUFFER_BYTES (~308 KB) which the S3's MMU
 * completes in well under a millisecond at 80 MHz PSRAM. The whole
 * point is to keep this cheap so it fits inside the inter-frame gap
 * of a continuous-PTT burst.
 *
 * @param src  pointer to a buffer holding NANOJS8_JS8_MODULATE_SAMPLE_COUNT
 *             int16 samples (typically a per-frame cache buffer the
 *             worker filled by calling modulate_text_frame() + reading
 *             nanojs8_js8_modulator_get_samples())
 * @param n    number of samples in src; MUST equal
 *             NANOJS8_JS8_MODULATE_SAMPLE_COUNT
 * @return  ESP_OK on success; ESP_ERR_INVALID_STATE if the modulator
 *          isn't initialized; ESP_ERR_INVALID_ARG if src is NULL;
 *          ESP_ERR_INVALID_SIZE if n is wrong.
 */
esp_err_t nanojs8_js8_modulator_load_samples(const int16_t *src, size_t n);

/**
 * Standalone self-test for the modulator (subtest C). Called from
 * nanojs8_js8_encode_self_test() when A and B pass — separated so the
 * orchestration stays in js8_encode.cpp while the modulator code lives
 * in pure C. Returns true on PASS; logs details either way.
 */
bool nanojs8_js8_modulate_self_test(void);

#ifdef __cplusplus
}
#endif
