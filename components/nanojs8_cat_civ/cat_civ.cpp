/*
 * cat_civ.cpp — Icom CI-V frame builder + parser implementation (L6b.5)
 * =======================================================================
 * Pure protocol code. No FreeRTOS, no driver calls, no allocation.
 * Every function operates on caller-supplied buffers so the
 * application owns memory lifecycle.
 *
 * License: GPL-3.0
 */

#include "cat_civ.h"

#include "esp_log.h"

static const char *TAG = "civ";

// Parser states. Kept as internal #defines so callers don't pick
// invalid values via the rx->state field.
#define RX_STATE_IDLE       0
#define RX_STATE_PREAMBLE   1   // saw one FE, waiting for the second
#define RX_STATE_COLLECTING 2   // saw FE FE, collecting payload till FD

// Minimum payload size to be a valid frame: to_addr + from_addr + cmd
// (3 bytes). Anything shorter and we can't even identify direction.
#define RX_MIN_PAYLOAD 3

extern "C" void nanojs8_civ_rx_init(nanojs8_civ_rx_t *rx, uint8_t ctrl_addr) {
    if (!rx) return;
    rx->len             = 0;
    rx->state           = RX_STATE_IDLE;
    rx->ctrl_addr       = ctrl_addr;
    rx->frames_ok       = 0;
    rx->frames_echoed   = 0;
    rx->frames_dropped  = 0;
}

extern "C" void nanojs8_civ_rx_feed(nanojs8_civ_rx_t *rx,
                                    uint8_t byte,
                                    nanojs8_civ_frame_handler_t handler,
                                    void *user_ctx) {
    if (!rx) return;

    switch (rx->state) {
    case RX_STATE_IDLE:
        if (byte == NANOJS8_CIV_PREAMBLE) {
            rx->state = RX_STATE_PREAMBLE;
        }
        // Any other byte while IDLE: junk between frames. Drop silently.
        return;

    case RX_STATE_PREAMBLE:
        if (byte == NANOJS8_CIV_PREAMBLE) {
            // Second FE — frame body starts after this.
            rx->state = RX_STATE_COLLECTING;
            rx->len   = 0;
        } else {
            // FE not followed by FE means the FE was probably noise.
            // Drop back to IDLE; if THIS byte itself is another FE
            // would-be preamble, we already wouldn't be here. Any other
            // byte is just discarded.
            rx->state = RX_STATE_IDLE;
        }
        return;

    case RX_STATE_COLLECTING:
        if (byte == NANOJS8_CIV_END) {
            // Frame complete. Validate length first.
            if (rx->len < RX_MIN_PAYLOAD) {
                ESP_LOGW(TAG, "Dropped malformed frame: len=%u < %u",
                         (unsigned)rx->len, (unsigned)RX_MIN_PAYLOAD);
                rx->frames_dropped++;
                rx->state = RX_STATE_IDLE;
                rx->len   = 0;
                return;
            }

            uint8_t to_addr   = rx->buf[0];
            uint8_t from_addr = rx->buf[1];
            uint8_t cmd       = rx->buf[2];

            // Echo filter: when CI-V is half-duplex (one wire to the
            // radio) every byte we send is also seen on RX. The echo
            // has from_addr == our controller addr. Drop it.
            if (from_addr == rx->ctrl_addr) {
                rx->frames_echoed++;
                // Don't log every echo at INFO — would spam the console
                // on every command. Use DEBUG so it's available when
                // wanted but quiet by default.
                ESP_LOGD(TAG, "Echo filtered (from=0x%02X)", from_addr);
                rx->state = RX_STATE_IDLE;
                rx->len   = 0;
                return;
            }

            // Real frame from the radio. Build the public view and call
            // the handler.
            nanojs8_civ_frame_t frame;
            frame.to_addr   = to_addr;
            frame.from_addr = from_addr;
            frame.cmd       = cmd;
            frame.data      = (rx->len > 3) ? &rx->buf[3] : NULL;
            frame.data_len  = rx->len - 3;
            rx->frames_ok++;
            ESP_LOGI(TAG, "RX frame: to=0x%02X from=0x%02X cmd=0x%02X data_len=%u",
                     to_addr, from_addr, cmd, (unsigned)frame.data_len);
            if (handler) handler(&frame, user_ctx);

            rx->state = RX_STATE_IDLE;
            rx->len   = 0;
            return;
        }

        if (byte == NANOJS8_CIV_PREAMBLE) {
            // FE inside a frame is illegal per CI-V spec. Most likely
            // we missed the FD terminator (line noise or buffer drop)
            // and this is the start of the next frame. Reset to
            // PREAMBLE state so a following FE resyncs us.
            ESP_LOGW(TAG, "Mid-frame FE — resyncing");
            rx->frames_dropped++;
            rx->state = RX_STATE_PREAMBLE;
            rx->len   = 0;
            return;
        }

        // Normal payload byte. Append if there's room.
        if (rx->len >= NANOJS8_CIV_MAX_FRAME) {
            // Oversize frame. Real CI-V frames top out around 15 bytes;
            // anything bigger is corruption or someone else's protocol
            // on the same wire. Drop and resync.
            ESP_LOGW(TAG, "Oversize frame; dropping (len=%u)",
                     (unsigned)rx->len);
            rx->frames_dropped++;
            rx->state = RX_STATE_IDLE;
            rx->len   = 0;
            return;
        }
        rx->buf[rx->len++] = byte;
        return;

    default:
        // Should be unreachable; defensive: reset.
        rx->state = RX_STATE_IDLE;
        rx->len   = 0;
        return;
    }
}

extern "C" size_t nanojs8_civ_build_read_freq(uint8_t *out, size_t out_cap,
                                              uint8_t radio_addr,
                                              uint8_t ctrl_addr) {
    if (!out || out_cap < 6) return 0;
    out[0] = NANOJS8_CIV_PREAMBLE;
    out[1] = NANOJS8_CIV_PREAMBLE;
    out[2] = radio_addr;
    out[3] = ctrl_addr;
    out[4] = NANOJS8_CIV_CMD_READ_FREQ;
    out[5] = NANOJS8_CIV_END;
    return 6;
}

extern "C" size_t nanojs8_civ_build_set_freq(uint8_t *out, size_t out_cap,
                                             uint8_t radio_addr,
                                             uint8_t ctrl_addr,
                                             uint64_t freq_hz) {
    if (!out || out_cap < 11) return 0;
    out[0] = NANOJS8_CIV_PREAMBLE;
    out[1] = NANOJS8_CIV_PREAMBLE;
    out[2] = radio_addr;
    out[3] = ctrl_addr;
    out[4] = NANOJS8_CIV_CMD_SET_FREQ;
    nanojs8_civ_freq_to_bcd(freq_hz, &out[5]);
    out[10] = NANOJS8_CIV_END;
    return 11;
}

extern "C" void nanojs8_civ_freq_to_bcd(uint64_t freq_hz, uint8_t out[5]) {
    if (!out) return;
    // Clamp to what 10 BCD digits can represent.
    if (freq_hz > 9999999999ULL) freq_hz = 9999999999ULL;
    // Extract digits LSB first.
    // d[i] is the i-th decimal digit (i=0 is ones place).
    uint8_t d[10];
    for (int i = 0; i < 10; ++i) {
        d[i] = (uint8_t)(freq_hz % 10);
        freq_hz /= 10;
    }
    // Pack pairs LSB-first: byte[k] holds digits d[2k+1]:d[2k].
    // The high nibble is the higher-place digit; low nibble is lower.
    for (int k = 0; k < 5; ++k) {
        out[k] = (uint8_t)((d[2*k + 1] << 4) | d[2*k]);
    }
}

extern "C" uint64_t nanojs8_civ_bcd_to_freq(const uint8_t bcd[5]) {
    if (!bcd) return 0;
    // Validate every nibble is 0-9 before computing. A non-BCD nibble
    // means corruption or we mis-parsed the frame — return 0 so the
    // caller knows not to use it.
    for (int k = 0; k < 5; ++k) {
        uint8_t lo = bcd[k] & 0x0F;
        uint8_t hi = (uint8_t)((bcd[k] >> 4) & 0x0F);
        if (lo > 9 || hi > 9) {
            ESP_LOGW(TAG, "Invalid BCD nibble at byte %d: 0x%02X",
                     k, bcd[k]);
            return 0;
        }
    }
    // Reassemble LSB-first.
    uint64_t hz = 0;
    uint64_t place = 1;
    for (int k = 0; k < 5; ++k) {
        uint8_t lo = bcd[k] & 0x0F;
        uint8_t hi = (uint8_t)((bcd[k] >> 4) & 0x0F);
        hz += lo * place;
        place *= 10;
        hz += hi * place;
        place *= 10;
    }
    return hz;
}
