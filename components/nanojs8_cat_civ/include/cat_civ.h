/*
 * cat_civ.h — NanoJS8 Icom CI-V frame builder + parser (L6b.5)
 * ==============================================================
 * Pure protocol layer. No hardware, no FreeRTOS, no state beyond what
 * the caller passes in. All bytes-in / bytes-out so this can be unit-
 * tested with stub buffers and reused for any CI-V radio (G90 today,
 * IC-705 / X6100 / FT-8x7 with CI-V mod tomorrow).
 *
 * Protocol summary
 * ────────────────
 * Frame on the wire:
 *     0xFE 0xFE <to_addr> <from_addr> <cmd> [subcmd] [data...] 0xFD
 *
 * Address bytes:
 *   - Radio: typically 0x70 (G90), 0x88, 0x94 (IC-705), 0xA4 (IC-7300)
 *   - Controller: 0xE0..0xEF, with 0xE0 conventionally the "any
 *     controller" address. We use 0xE0.
 *
 * Special bytes in the payload:
 *   - 0xFE / 0xFD never appear in a valid payload — both are framing
 *     bytes and the protocol guarantees BCD/command data won't collide
 *     with them. (BCD nibbles are 0-9, max 0x99. Command bytes are
 *     0x00-0x2F, 0xFA/0xFB/0xFC are status codes.)
 *
 * Frequency encoding (Icom 5-byte BCD, little-endian):
 *   - 10 decimal digits packed two-per-byte
 *   - byte[0] has digits d1:d0 (LSB pair)
 *   - byte[4] has digits d9:d8 (MSB pair)
 *   - Example: 14,074,000 Hz → "0014074000" → 00 40 07 14 00
 *
 * Half-duplex echoes
 * ──────────────────
 * CI-V is electrically half-duplex: when we transmit, our own bytes
 * are echoed back through the RX path. The standard filter is to drop
 * any frame where ``from_addr == our_controller_addr``. We do that in
 * nanojs8_civ_rx_feed() so the application never sees its own echoes.
 *
 * State machine for the parser
 * ────────────────────────────
 * The parser is a simple Mealy machine driven by a byte at a time.
 * No allocation, no callbacks owned by the parser — the user passes
 * a frame_handler each call.
 *
 *   IDLE       --0xFE--> PREAMBLE
 *   IDLE       --other-> IDLE
 *   PREAMBLE   --0xFE--> COLLECTING  (clear buf)
 *   PREAMBLE   --other-> IDLE
 *   COLLECTING --0xFD--> emit frame; IDLE
 *   COLLECTING --0xFE--> PREAMBLE    (recover from junk)
 *   COLLECTING --buf full-> IDLE     (oversize frame; drop)
 *   COLLECTING --other-> append byte
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// CI-V framing constants.
#define NANOJS8_CIV_PREAMBLE 0xFE
#define NANOJS8_CIV_END      0xFD

// Common CI-V commands NanoJS8 uses.
#define NANOJS8_CIV_CMD_SET_FREQ      0x05   // (followed by 5 BCD bytes)
#define NANOJS8_CIV_CMD_READ_FREQ     0x03   // (no data; reply contains BCD)

// Status responses (single-byte commands the radio sends back).
#define NANOJS8_CIV_STATUS_OK         0xFB   // command accepted
#define NANOJS8_CIV_STATUS_NG         0xFA   // command rejected

// Maximum frame size (FE FE to from cmd <data> FD). 16 bytes is way
// more than any frame we issue or expect; oversize frames are dropped.
#define NANOJS8_CIV_MAX_FRAME 32

// A parsed CI-V frame. The buffer contains the bytes BETWEEN the
// FE FE preamble and the FD terminator (exclusive of both). So
//   raw on wire:  FE FE 70 E0 03 FD                 (6 bytes)
//   payload here: 70 E0 03                          (3 bytes)
//   to_addr = 0x70, from_addr = 0xE0, cmd = 0x03,
//   data = empty
typedef struct {
    uint8_t to_addr;     // payload[0]
    uint8_t from_addr;   // payload[1]
    uint8_t cmd;         // payload[2]
    const uint8_t *data; // payload[3..len-1] — points into the rx buf;
                         // valid only for the duration of the handler call
    size_t  data_len;
} nanojs8_civ_frame_t;

// Frame handler signature. Called from nanojs8_civ_rx_feed() when a
// complete, non-echo frame arrives. The pointer in frame->data is only
// valid for the duration of this call.
typedef void (*nanojs8_civ_frame_handler_t)(const nanojs8_civ_frame_t *frame,
                                            void *user_ctx);

// Parser state. Caller allocates one of these per CI-V port.
typedef struct {
    uint8_t  buf[NANOJS8_CIV_MAX_FRAME];
    uint16_t len;       // bytes in buf (0..MAX_FRAME)
    uint8_t  state;     // see RX_STATE_* below
    uint8_t  ctrl_addr; // OUR controller addr (used for echo filtering)
    // Diagnostic counters — operator-visible via nanojs8_civ_rx_stats().
    // Wrap at uint32_t max (>4 billion frames; not a concern at CI-V
    // rates) so no special handling needed for overflow.
    uint32_t frames_ok;        // valid frames delivered to handler
    uint32_t frames_echoed;    // filtered as our own echo
    uint32_t frames_dropped;   // oversize or malformed
} nanojs8_civ_rx_t;

// Initialize/reset a parser. Must be called once before the first
// _feed(). ctrl_addr is OUR controller address — used to filter our
// own echoed-back commands.
void nanojs8_civ_rx_init(nanojs8_civ_rx_t *rx, uint8_t ctrl_addr);

// Feed a single received byte into the parser. May trigger zero or
// one handler invocation. handler may be NULL (then the parser still
// runs and counters update; useful during init before app is ready).
void nanojs8_civ_rx_feed(nanojs8_civ_rx_t *rx,
                         uint8_t byte,
                         nanojs8_civ_frame_handler_t handler,
                         void *user_ctx);

// Build a "read frequency" frame into out. Returns the number of bytes
// written. out must have room for ≥6 bytes. Format:
//     FE FE <radio_addr> <ctrl_addr> 03 FD
size_t nanojs8_civ_build_read_freq(uint8_t *out, size_t out_cap,
                                   uint8_t radio_addr, uint8_t ctrl_addr);

// Build a "set frequency" frame into out. Returns the number of bytes
// written. out must have room for ≥11 bytes. Format:
//     FE FE <radio_addr> <ctrl_addr> 05 <bcd[5]> FD
// The frequency is encoded as 5-byte little-endian BCD per the Icom spec.
size_t nanojs8_civ_build_set_freq(uint8_t *out, size_t out_cap,
                                  uint8_t radio_addr, uint8_t ctrl_addr,
                                  uint64_t freq_hz);

// Encode a frequency as 5 BCD bytes (little-endian). out must be ≥5
// bytes. freq_hz is clamped to 9,999,999,999 Hz (the maximum the 10
// BCD digits can represent — well above any HF/VHF/UHF frequency).
void nanojs8_civ_freq_to_bcd(uint64_t freq_hz, uint8_t out[5]);

// Decode 5 BCD bytes (little-endian) into a frequency in Hz. Returns
// 0 if any nibble is not a valid BCD digit (0-9). Caller should check
// the result against expected ranges (a 0 Hz response is almost
// certainly a parse error, not a real radio state).
uint64_t nanojs8_civ_bcd_to_freq(const uint8_t bcd[5]);

#ifdef __cplusplus
}
#endif
