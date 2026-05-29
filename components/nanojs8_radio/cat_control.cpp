// NanoJS8 — CAT control implementation for (tr)uSDX.
//
// See cat_control.h for the protocol overview. This file implements the
// command send/parse layer over an already-open CdcAcmDevice* (the
// CH340 CDC port of the (tr)uSDX).
//
// RX model: CDC bytes arrive asynchronously via the device's data_cb.
// We accumulate them into a small ring/line buffer and parse complete
// ';'-terminated CAT frames. For the slow-poll status query we send IF;
// then wait briefly for the matching reply to land in the buffer.
//
// This module is single-writer: only the radio service's CAT task calls
// set_ptt()/poll_status()/bind(). get_state() is a lock-free reader for
// the UI task. The latest decoded state is published via atomics.

#include "cat_control.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/cdc_acm_host.h"

namespace nanojs8 {
namespace radio {
namespace cat {

static const char* TAG = "radio_cat";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static CdcAcmDevice*        s_dev = nullptr;   // borrowed; radio service owns lifecycle

// Published state (lock-free for UI reads).
static std::atomic<bool>     s_valid{false};
static std::atomic<uint64_t> s_freq_hz{0};
static std::atomic<uint8_t>  s_mode{(uint8_t)Mode::UNKNOWN};
static std::atomic<bool>     s_tx{false};

// RX accumulation buffer, filled by the CDC data callback. CAT frames
// are short (IF; reply is ~38 bytes). 256 is ample. We treat it as a
// simple byte FIFO drained by the parser.
static constexpr size_t RX_BUF_SIZE = 256;
static uint8_t           s_rx_buf[RX_BUF_SIZE];
static volatile size_t   s_rx_len = 0;         // bytes currently in s_rx_buf
static portMUX_TYPE      s_rx_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::LSB: return "LSB";
        case Mode::USB: return "USB";
        case Mode::CW:  return "CW";
        case Mode::FM:  return "FM";
        case Mode::AM:  return "AM";
        default:        return "---";
    }
}

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

// CDC RX callback: append received bytes to s_rx_buf. Runs in the
// cdc_acm client task context. Keep it minimal — just buffer, parse
// later in poll_status() on the CAT task.
static bool cat_rx_cb(const uint8_t* data, size_t len, void* arg) {
    (void)arg;
    portENTER_CRITICAL(&s_rx_mux);
    for (size_t i = 0; i < len; ++i) {
        if (s_rx_len < RX_BUF_SIZE) {
            s_rx_buf[s_rx_len++] = data[i];
        } else {
            // Overflow: drop oldest half to keep the most recent bytes.
            // CAT frames are short; this only happens if we fall behind
            // badly, which shouldn't occur on the slow poll.
            memmove(s_rx_buf, s_rx_buf + RX_BUF_SIZE/2, RX_BUF_SIZE/2);
            s_rx_len = RX_BUF_SIZE/2;
            s_rx_buf[s_rx_len++] = data[i];
        }
    }
    portEXIT_CRITICAL(&s_rx_mux);
    return true;  // we consumed the data
}

// Drain a copy of the RX buffer for parsing, then clear it.
static size_t drain_rx(uint8_t* dst, size_t dst_cap) {
    portENTER_CRITICAL(&s_rx_mux);
    size_t n = s_rx_len;
    if (n > dst_cap) n = dst_cap;
    memcpy(dst, s_rx_buf, n);
    s_rx_len = 0;
    portEXIT_CRITICAL(&s_rx_mux);
    return n;
}

static void clear_rx() {
    portENTER_CRITICAL(&s_rx_mux);
    s_rx_len = 0;
    portEXIT_CRITICAL(&s_rx_mux);
}

// Send a CAT command string (must include the trailing ';').
static esp_err_t send_cmd(const char* cmd) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    const size_t len = strlen(cmd);
    // tx_blocking takes non-const uint8_t*; cmd is a string literal or
    // local buffer we own, so a const_cast is safe here.
    return s_dev->tx_blocking((uint8_t*)cmd, len, 100 /*ms*/);
}

// Find a CAT frame with the given 2-char prefix in buf[0..len), return
// pointer to the start of its payload (after the 2-char prefix) and set
// *payload_len to the number of payload chars before the ';'. Returns
// nullptr if no complete matching frame is present.
static const char* find_frame(const uint8_t* buf, size_t len,
                              const char* prefix, size_t* payload_len) {
    const size_t plen = strlen(prefix);
    for (size_t i = 0; i + plen < len; ++i) {
        if (memcmp(buf + i, prefix, plen) == 0) {
            // Find the terminating ';'.
            for (size_t j = i + plen; j < len; ++j) {
                if (buf[j] == ';') {
                    *payload_len = j - (i + plen);
                    return (const char*)(buf + i + plen);
                }
            }
            return nullptr;  // prefix found but frame not yet complete
        }
    }
    return nullptr;
}

// Wait up to timeout_ms for a complete CAT frame with `prefix` to
// arrive. Accumulates bytes across poll iterations into `scratch` so a
// reply split across multiple USB bursts is still parsed correctly.
// Returns the payload pointer into `scratch`, or null on timeout.
static const char* read_reply(const char* prefix, size_t* payload_len,
                              uint8_t* scratch, size_t scratch_cap,
                              uint32_t timeout_ms) {
    const int64_t deadline = now_ms() + timeout_ms;
    size_t acc = 0;   // bytes accumulated in scratch so far
    while (now_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
        // Append any newly-received bytes to the accumulator.
        if (acc < scratch_cap) {
            acc += drain_rx(scratch + acc, scratch_cap - acc);
        } else {
            // Accumulator full without a match — shift out the older
            // half so we can keep absorbing (CAT frames are short; this
            // only triggers on a flood of unrelated bytes).
            memmove(scratch, scratch + scratch_cap/2, scratch_cap/2);
            acc = scratch_cap/2;
            acc += drain_rx(scratch + acc, scratch_cap - acc);
        }
        if (acc == 0) continue;
        const char* p = find_frame(scratch, acc, prefix, payload_len);
        if (p) return p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t bind(CatDeviceHandle dev) {
    s_dev = static_cast<CdcAcmDevice*>(dev);
    s_valid.store(false, std::memory_order_release);
    s_freq_hz.store(0, std::memory_order_release);
    s_mode.store((uint8_t)Mode::UNKNOWN, std::memory_order_release);
    s_tx.store(false, std::memory_order_release);
    clear_rx();

    if (!s_dev) return ESP_ERR_INVALID_ARG;

    // ------------------------------------------------------------------
    // (tr)uSDX init sequence — derived from the reference Python driver
    // (olgierd/trusdx-audio, SQ3SWF 2023). The (tr)uSDX is an Atmega328
    // running CAT alongside DSP/audio/RF/display, and it has two startup
    // quirks that bit us before we understood them:
    //
    // QUIRK 1: 3-second post-open settle delay.
    //   After the serial port opens, the radio's firmware needs ~3 s
    //   before it will respond to ANY CAT command. Sending ID; before
    //   that window times out silently. The reference Python does this:
    //
    //       ser = serial.Serial(...)
    //       time.sleep(3)        # wait for device to start
    //       ser.write(b"UA1;")   # NOW send commands
    //
    //   Without this wait we see "No CAT ID; reply within 500 ms" on
    //   cold sessions even though the radio is fine.
    //
    // QUIRK 2: Streaming state (UA0/UA1/UA2) is sticky across sessions.
    //   If a prior application (WSJT-X, JS8Call, our own audio code in
    //   step-2) left the radio in UA1; (audio streaming on), the radio
    //   sends continuous audio bytes the moment we open the port.
    //   Our ID; query gets buried in the audio stream and times out.
    //   ~30-60 s later the (tr)uSDX's small Atmega328 USB buffer fills
    //   up, its firmware self-resets the USB stack to recover, and we
    //   see a CDC disconnect of mysterious cause. That was the source
    //   of the disconnects we observed in v0.5.0/0.5.1/0.5.2 testing.
    //
    // FIX: wait 3 s, then explicitly send UA0; to force CAT-only mode
    // before any other command. UA0; is idempotent (no-op if already
    // off) so it's safe regardless of prior state.
    // ------------------------------------------------------------------

    ESP_LOGI(TAG, "Waiting 3 s for (tr)uSDX firmware to be ready...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Force CAT-only mode (clear any leftover UA1;/UA2; from prior
    // sessions). No reply expected; we just discard whatever's in the
    // buffer immediately afterward to drop any stale audio bytes that
    // arrived during the settle wait.
    esp_err_t err = send_cmd("UA0;");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UA0; send failed: %s — continuing", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Sent UA0; (force CAT-only mode)");
    }
    // Give the radio a moment to process UA0; and stop any audio stream,
    // then clear the RX buffer of stale bytes accumulated during settle.
    vTaskDelay(pdMS_TO_TICKS(200));
    clear_rx();

    // Handshake: send ID; and expect "ID020;". The (tr)uSDX emulates a
    // TS-480 (id 020). A wrong/absent id isn't fatal — we log and let
    // the caller decide — but it's a useful sanity check that we're
    // actually talking to a (tr)uSDX and not some other CH340 device.
    err = send_cmd("ID;");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ID; send failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t scratch[RX_BUF_SIZE];
    size_t plen = 0;
    const char* p = read_reply("ID", &plen, scratch, sizeof(scratch), 500);
    if (p && plen >= 3 && memcmp(p, "020", 3) == 0) {
        ESP_LOGI(TAG, "CAT handshake OK — (tr)uSDX (TS-480 id 020)");
        return ESP_OK;
    }
    if (p) {
        ESP_LOGW(TAG, "CAT id unexpected (got %.*s, wanted 020) — proceeding",
                 (int)plen, p);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "No CAT ID; reply within 500 ms — proceeding anyway");
    return ESP_ERR_INVALID_RESPONSE;
}

void unbind(void) {
    // Graceful unbind: leave the radio in a clean state for the next
    // session by sending UA0; so any streaming we enabled is disabled.
    // Caller should only invoke this path when the device is still
    // healthy (e.g. radio_service::stop()). For the disconnect path
    // where the device is already invalidated, use unbind_forced().
    if (s_dev) {
        const esp_err_t err = send_cmd("UA0;");
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sent UA0; on unbind (leave radio in CAT-only state)");
        } else {
            ESP_LOGD(TAG, "UA0; on unbind failed: %s", esp_err_to_name(err));
        }
        // Brief settle so the radio actually processes the command before
        // the device handle goes away under us.
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_dev = nullptr;
    s_valid.store(false, std::memory_order_release);
    clear_rx();
}

void unbind_forced(void) {
    // Forced unbind: device is already gone (USB disconnect, cable pull,
    // (tr)uSDX firmware reset). Do NOT attempt any I/O — tx_blocking on
    // an invalidated device would error or block, and we're typically
    // running in the cdc_acm client event-callback context where blocking
    // is harmful. Just zero our state and let the device handle be freed
    // by the cdc_acm driver.
    s_dev = nullptr;
    s_valid.store(false, std::memory_order_release);
    clear_rx();
}

esp_err_t set_ptt(bool tx) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    // TX0; = transmit (data/SSB), RX; = receive. We use TX0; (not TX2;,
    // which is CW tune). The (tr)uSDX returns to RX on RX;.
    const esp_err_t err = send_cmd(tx ? "TX0;" : "RX;");
    if (err == ESP_OK) {
        s_tx.store(tx, std::memory_order_release);
        ESP_LOGI(TAG, "CAT PTT -> %s", tx ? "TX" : "RX");
    } else {
        ESP_LOGE(TAG, "CAT PTT %s failed: %s", tx ? "TX0;" : "RX;",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t poll_status(void) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    // Use FA; (frequency) and MD; (mode) — two short, unambiguous
    // queries. IF; returns both but its fixed-width layout varies
    // slightly between TS-480 emulations; FA;/MD; are simpler to parse
    // robustly. Two tiny queries at 115200 are still negligible.
    uint8_t scratch[RX_BUF_SIZE];
    size_t  plen = 0;

    // --- Frequency: FA; -> "FA" + 11 digits + ";" ---
    clear_rx();
    esp_err_t err = send_cmd("FA;");
    if (err != ESP_OK) return err;
    const char* fp = read_reply("FA", &plen, scratch, sizeof(scratch), 300);
    bool got_any = false;
    if (fp && plen >= 1 && plen <= 11) {
        // Parse decimal digits into Hz.
        uint64_t hz = 0;
        bool ok = true;
        for (size_t i = 0; i < plen; ++i) {
            if (fp[i] < '0' || fp[i] > '9') { ok = false; break; }
            hz = hz * 10 + (uint64_t)(fp[i] - '0');
        }
        if (ok) {
            s_freq_hz.store(hz, std::memory_order_release);
            got_any = true;
        }
    }

    // --- Mode: MD; -> "MD" + 1 digit + ";" ---
    clear_rx();
    err = send_cmd("MD;");
    if (err != ESP_OK) return err;
    const char* mp = read_reply("MD", &plen, scratch, sizeof(scratch), 300);
    if (mp && plen >= 1) {
        const char d = mp[0];
        Mode m = Mode::UNKNOWN;
        switch (d) {
            case '1': m = Mode::LSB; break;
            case '2': m = Mode::USB; break;
            case '3': m = Mode::CW;  break;
            case '4': m = Mode::FM;  break;
            case '5': m = Mode::AM;  break;
            default:  m = Mode::UNKNOWN; break;
        }
        s_mode.store((uint8_t)m, std::memory_order_release);
        got_any = true;
    }

    if (got_any) {
        s_valid.store(true, std::memory_order_release);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

void get_state(CatState* out) {
    if (!out) return;
    out->valid        = s_valid.load(std::memory_order_acquire);
    out->freq_hz      = s_freq_hz.load(std::memory_order_acquire);
    out->mode         = (Mode)s_mode.load(std::memory_order_acquire);
    out->transmitting = s_tx.load(std::memory_order_acquire);
}

// Expose the RX callback so the radio service can wire it into the
// Ch34x device config when it opens the port.
cdc_acm_data_callback_t get_rx_callback(void) {
    return cat_rx_cb;
}

} // namespace cat
} // namespace radio
} // namespace nanojs8
