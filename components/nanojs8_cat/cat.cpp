/*
 * cat.cpp — NanoJS8 CAT facade implementation (L6b.5)
 * =====================================================
 * See cat.h for the public API and design notes.
 *
 * Thread safety
 * ─────────────
 * The RX callback runs on the USB host task (per nanojs8_usb_serial
 * documentation: not the main loop, may not call back into the
 * driver). Public CAT functions are called from the main app loop.
 * State that crosses these contexts (last_freq_hz, last_reply_ms,
 * status, parser counters) is read by the app and written by the RX
 * callback. We use std::atomic for the simple scalars to avoid torn
 * reads. The CI-V parser struct itself is only ever touched on the
 * RX path so it doesn't need locking.
 *
 * Profile changes happen from SETUP commit (main loop). We update
 * s_profile under an atomic pointer load/store; the RX path reads
 * the pointer once at the top of each callback and processes the
 * batch with that snapshot. A late-arriving response after a profile
 * change is treated as belonging to the previous profile and dropped
 * by the address mismatch (different radio_addr ⇒ unexpected from
 * field ⇒ we ignore).
 *
 * Timeout handling
 * ────────────────
 * We track the time of the last sent request (s_last_tx_ms) and the
 * time of the last received response (s_last_reply_ms). status() is
 * a derived value:
 *   - profile == CAT_NONE                          → OFF
 *   - last_reply_ms within REPLY_FRESH_MS          → OK
 *   - last_tx_ms within REPLY_TIMEOUT_MS (waiting) → WAITING
 *   - otherwise                                    → NO_REPLY
 *
 * This means "I haven't asked anything in a while" doesn't show as
 * an error — only "I asked and didn't get an answer" does. That's
 * the right semantic for a passive UI row.
 *
 * License: GPL-3.0
 */

#include "cat.h"
#include "cat_civ.h"
#include "usb_serial.h"
#include "radio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include <string.h>
#include <inttypes.h>
#include <atomic>

static const char *TAG = "cat";

// How long a recent response stays "fresh" (status() returns OK).
// 30 seconds is generous — the radio isn't going anywhere between
// our polls, and JS8 slot cadence is 15 s, so this lets a single
// per-slot poll keep status OK indefinitely.
static constexpr uint32_t REPLY_FRESH_MS = 30000;

// How long we wait after sending a request before declaring NO_REPLY.
// G90 round-trip at 19200 baud is well under 100 ms for a 6-byte
// command + 11-byte response. 1.5 s covers any reasonable hiccup
// without making the UI feel unresponsive.
static constexpr uint32_t REPLY_TIMEOUT_MS = 1500;

// L6b.6 fix2: periodic CAT poll cadence. While the active profile is
// CAT_CIV and serial is ready, nanojs8_cat_tick() fires a read_freq
// every CAT_POLL_INTERVAL_MS to (a) keep s_last_reply_ms rolling so
// status() stays OK on HOME, and (b) detect manual retunes the
// operator made on the radio's own dial. 10 s is a reasonable
// trade-off: tight enough that HOME catches manual tuning within
// one JS8 slot, loose enough that we're not flooding the CAT bus.
// Polling pauses while a TX is in flight (last_tx_ms within
// REPLY_TIMEOUT_MS) and resumes after.
static constexpr uint32_t CAT_POLL_INTERVAL_MS = 10000;

namespace {

// Active profile pointer. Set on apply_profile, read on every TX/RX.
// std::atomic so the RX callback (USB host task) sees consistent
// values when the main loop swaps profiles via SETUP commit.
std::atomic<const nanojs8_radio_profile_t *> s_profile{nullptr};

// Cached state — all atomic for cross-task reads.
std::atomic<uint64_t> s_last_freq_hz{0};
std::atomic<uint32_t> s_last_reply_ms{0};
std::atomic<uint32_t> s_last_tx_ms{0};
std::atomic<uint32_t> s_tx_count{0};
// True after the first apply_profile(); false at boot. Lets status()
// return OFF cleanly before init completes.
std::atomic<bool> s_started{false};

// L6b.6: deferred-probe flag. Set by apply_profile() for CAT_CIV
// profiles. nanojs8_cat_tick() (called from the main loop) fires the
// probe and clears the flag once the serial layer is connected. This
// handles the boot-time race where apply_profile happens before the
// CP2102 has enumerated.
std::atomic<bool> s_want_initial_probe{false};

// L6b.6 build-fix2: optimistic "freq we just told the radio to tune to,
// pending FB ack". The Xiegu G90 (and some other Icom-protocol radios)
// responds to set_freq with a bare 0xFB OK acknowledgement — no freq
// echo via 0x05 — so without this we never know to update s_last_freq_hz
// after a set. Set in nanojs8_cat_set_freq() before TX, consumed in the
// FB-ack branch of the RX handler. 0 means "no set pending".
std::atomic<uint64_t> s_pending_set_freq_hz{0};

// L6b.6 build-fix2: defensive verify-after-set. After we copy the
// pending freq into s_last_freq_hz on FB ack, we also queue a follow-up
// read_freq so the displayed value reflects what the RADIO reports
// rather than just what we asked for. Catches the rare case where a
// radio "accepts" a freq but tunes to something slightly different
// (10 Hz step rounding, band-edge clipping). nanojs8_cat_tick() picks
// up this flag and fires the read on its next iteration.
std::atomic<bool> s_want_verify_read{false};

// CI-V parser state. Only touched on the RX callback path.
nanojs8_civ_rx_t s_civ_rx;

// Frame handler — called from inside the CI-V parser when a complete
// non-echo frame is recognized. Lives in the RX callback context.
void on_civ_frame(const nanojs8_civ_frame_t *frame, void *) {
    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p) return;

    // Drop frames not addressed to us OR from a different radio than
    // the active profile expects. This catches stragglers after a
    // profile change and any frames from other devices on the same wire.
    if (frame->from_addr != p->cat_civ_radio_addr) {
        ESP_LOGW(TAG, "Ignoring frame from 0x%02X (expected 0x%02X)",
                 frame->from_addr, p->cat_civ_radio_addr);
        return;
    }
    // to_addr can be either our specific ctrl_addr OR 0x00 (broadcast).
    // The G90 normally addresses replies to our exact ctrl_addr, but
    // some firmware uses 0x00 for unsolicited transceive updates.
    if (frame->to_addr != p->cat_civ_ctrl_addr && frame->to_addr != 0x00) {
        ESP_LOGW(TAG, "Ignoring frame to 0x%02X (expected 0x%02X or 0x00)",
                 frame->to_addr, p->cat_civ_ctrl_addr);
        return;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    switch (frame->cmd) {
    case NANOJS8_CIV_CMD_READ_FREQ:
    case NANOJS8_CIV_CMD_SET_FREQ: {
        // Both commands carry the current/just-set frequency in the
        // 5-byte BCD data field. (Icom radios echo back the new freq
        // after a set, which is how we confirm the radio accepted it.)
        if (frame->data_len != 5) {
            ESP_LOGW(TAG, "Freq frame has data_len=%u (expected 5)",
                     (unsigned)frame->data_len);
            return;
        }
        uint64_t hz = nanojs8_civ_bcd_to_freq(frame->data);
        if (hz == 0) {
            // Parse error already logged inside bcd_to_freq.
            return;
        }
        uint64_t prev = s_last_freq_hz.exchange(hz, std::memory_order_release);
        s_last_reply_ms.store(now_ms, std::memory_order_release);
        if (prev != hz) {
            ESP_LOGI(TAG, "Freq update: %llu Hz -> %llu Hz",
                     (unsigned long long)prev, (unsigned long long)hz);
        } else {
            ESP_LOGI(TAG, "Freq confirmed: %llu Hz", (unsigned long long)hz);
        }
        return;
    }

    case NANOJS8_CIV_STATUS_OK: {
        // Command accepted (typically the OK for a set_freq we just
        // sent). Update reply timestamp so status() stays OK.
        s_last_reply_ms.store(now_ms, std::memory_order_release);

        // L6b.6 build-fix2: if there's a pending set_freq value, the
        // FB ack confirms the radio accepted our value — copy it into
        // s_last_freq_hz so the UI shows what we set instead of the
        // stale read-back from boot. Use exchange(0) so concurrent
        // set_freq calls don't race (worst case: we miss this update
        // and the verify-read catches it).
        uint64_t pending = s_pending_set_freq_hz.exchange(
            0, std::memory_order_acq_rel);
        if (pending != 0) {
            uint64_t prev = s_last_freq_hz.exchange(
                pending, std::memory_order_release);
            ESP_LOGI(TAG, "Radio acknowledged set_freq OK (FB): "
                          "%llu Hz -> %llu Hz (optimistic from set; "
                          "verify-read queued)",
                     (unsigned long long)prev,
                     (unsigned long long)pending);
            // Queue a verify-read so the displayed value reflects the
            // radio's actual freq if it differs (10 Hz step rounding,
            // band-edge clipping, etc.). nanojs8_cat_tick() picks this
            // up on its next iteration — keeps the RX-callback context
            // free of TX side effects.
            s_want_verify_read.store(true, std::memory_order_release);
        } else {
            ESP_LOGI(TAG, "Radio acknowledged OK (FB) — no set pending");
        }
        return;
    }

    case NANOJS8_CIV_STATUS_NG:
        // Command rejected. The most likely cause is an unsupported
        // command or a value out of range. Log loudly but don't change
        // any cached state — the operator may want to retry.
        ESP_LOGW(TAG, "Radio rejected command (FA)");
        return;

    default:
        ESP_LOGI(TAG, "Unhandled CI-V cmd 0x%02X (data_len=%u)",
                 frame->cmd, (unsigned)frame->data_len);
        return;
    }
}

// Serial RX callback — called from the USB host task when bytes
// arrive. We feed each byte through the CI-V parser; the parser
// will invoke on_civ_frame when a complete frame is recognized.
bool on_serial_rx(const uint8_t *data, size_t len, void * /*arg*/) {
    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p || p->cat != NANOJS8_RADIO_CAT_CIV) {
        // No active CI-V profile — discard bytes silently. (RTS-only
        // profiles shouldn't have a radio sending us anything, but
        // hot-plug transitions can race; better to drop than crash.)
        return true;
    }
    for (size_t i = 0; i < len; ++i) {
        nanojs8_civ_rx_feed(&s_civ_rx, data[i], on_civ_frame, nullptr);
    }
    return true;
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

extern "C" esp_err_t nanojs8_cat_start(void) {
    if (s_started.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Already started — ignoring duplicate start");
        return ESP_OK;
    }
    // Register RX callback first so we don't miss bytes that arrive
    // during the apply_profile baud-set.
    esp_err_t err = nanojs8_serial_set_rx_callback(on_serial_rx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register serial RX callback: %s",
                 esp_err_to_name(err));
        return err;
    }
    // Apply the currently-active profile (may be CAT_NONE — that's fine).
    nanojs8_cat_apply_profile(nanojs8_radio_get_active());
    s_started.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "CAT facade started");
    return ESP_OK;
}

extern "C" void nanojs8_cat_apply_profile(const nanojs8_radio_profile_t *profile) {
    if (!profile) {
        ESP_LOGW(TAG, "apply_profile(NULL) — ignoring");
        return;
    }
    const nanojs8_radio_profile_t *prev =
        s_profile.exchange(profile, std::memory_order_acq_rel);

    // Reset CI-V parser with the new controller address (echo filter
    // needs to be right for the new profile).
    nanojs8_civ_rx_init(&s_civ_rx, profile->cat_civ_ctrl_addr);

    // Clear cached state on every profile change so the UI doesn't
    // show stale freq from the previous radio.
    s_last_freq_hz.store(0, std::memory_order_release);
    s_last_reply_ms.store(0, std::memory_order_release);
    s_last_tx_ms.store(0, std::memory_order_release);
    s_tx_count.store(0, std::memory_order_release);
    // L6b.6 build-fix2: pending set_freq from a previous profile is no
    // longer relevant — different radio addr means any in-flight FB
    // wouldn't pass our to_addr check anyway. Be explicit.
    s_pending_set_freq_hz.store(0, std::memory_order_release);
    s_want_verify_read.store(false, std::memory_order_release);

    if (profile->cat == NANOJS8_RADIO_CAT_NONE) {
        ESP_LOGI(TAG, "Profile '%s' has no CAT (CAT_NONE) — CAT is OFF",
                 profile->id);
        // Don't bother changing baud — the line baud is irrelevant
        // when nothing is sent/received over CAT. (RTS PTT works
        // regardless of baud setting.)
        return;
    }

    if (profile->cat == NANOJS8_RADIO_CAT_CIV) {
        // Set the serial line to the profile's CAT baud, 8N1.
        // G90 default is 19200 8N1, matching MicroJS8.
        //
        // If this fails (typically ESP_ERR_INVALID_STATE because the
        // CP2102 isn't connected yet), we log a warning and proceed.
        // The next apply_profile() call — triggered either by another
        // SETUP commit or by the operator re-selecting the same profile
        // — will retry. There is intentionally no automatic retry loop
        // here: a stuck-disconnected serial port should be visible as
        // "Serial: waiting for CP2102..." on HOME, not papered over.
        esp_err_t err = nanojs8_serial_set_line(
            profile->cat_baud, 8, NANOJS8_SERIAL_STOP_1, NANOJS8_SERIAL_PARITY_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "serial_set_line(%u 8N1) failed: %s "
                          "(will retry baud + initial probe when serial is ready)",
                     (unsigned)profile->cat_baud, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Profile '%s' CAT=CI-V @ %u 8N1, "
                          "radio=0x%02X ctrl=0x%02X "
                          "(was '%s')",
                     profile->id,
                     (unsigned)profile->cat_baud,
                     profile->cat_civ_radio_addr,
                     profile->cat_civ_ctrl_addr,
                     prev ? prev->id : "<none>");
        }
        // L6b.6: queue an initial probe regardless of whether set_line
        // worked. nanojs8_cat_tick() will retry the line config and
        // fire the probe once the serial layer reports READY. If the
        // serial layer is already ready here, the very next tick fires
        // the probe with ~0 ms latency.
        s_want_initial_probe.store(true, std::memory_order_release);
        return;
    }

    ESP_LOGW(TAG, "Unknown cat enum value %d for profile '%s'",
             (int)profile->cat, profile->id);
}

extern "C" nanojs8_cat_status_t nanojs8_cat_status(void) {
    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p || p->cat == NANOJS8_RADIO_CAT_NONE) {
        return NANOJS8_CAT_STATUS_OFF;
    }
    uint32_t now_ms     = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t reply_ms   = s_last_reply_ms.load(std::memory_order_acquire);
    uint32_t tx_ms      = s_last_tx_ms.load(std::memory_order_acquire);
    // OK if a reply arrived recently.
    if (reply_ms != 0 && (now_ms - reply_ms) <= REPLY_FRESH_MS) {
        return NANOJS8_CAT_STATUS_OK;
    }
    // WAITING if we have a request in flight that hasn't timed out yet.
    if (tx_ms != 0 && (now_ms - tx_ms) <= REPLY_TIMEOUT_MS) {
        // A reply might still arrive — call it WAITING. The status
        // promotes back to OK as soon as the response lands.
        return NANOJS8_CAT_STATUS_WAITING;
    }
    // We've sent something and not heard back within the timeout, OR
    // we've never sent anything. The latter case (clean boot, no
    // requests yet) is debatable — could be OFF or NO_REPLY. We pick
    // NO_REPLY because if CAT is enabled the operator expects to see
    // a freq sooner or later; "no reply" prompts them to press F to
    // probe rather than assume CAT just isn't being used.
    return NANOJS8_CAT_STATUS_NO_REPLY;
}

extern "C" uint64_t nanojs8_cat_last_freq_hz(void) {
    return s_last_freq_hz.load(std::memory_order_acquire);
}

extern "C" uint32_t nanojs8_cat_last_reply_ms(void) {
    return s_last_reply_ms.load(std::memory_order_acquire);
}

extern "C" bool nanojs8_cat_request_freq(void) {
    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p || p->cat != NANOJS8_RADIO_CAT_CIV) {
        ESP_LOGI(TAG, "request_freq: skip (profile cat != CIV)");
        return false;
    }
    uint8_t frame[6];
    size_t n = nanojs8_civ_build_read_freq(
        frame, sizeof(frame), p->cat_civ_radio_addr, p->cat_civ_ctrl_addr);
    if (n == 0) return false;

    // Verbose hex log for first-flight debugging — easy to grep:
    //   CAT TX: FE FE 70 E0 03 FD
    ESP_LOGI(TAG, "CAT TX: %02X %02X %02X %02X %02X %02X",
             frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);

    esp_err_t err = nanojs8_serial_write(frame, n, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request_freq: serial_write failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    s_last_tx_ms.store((uint32_t)(esp_timer_get_time() / 1000),
                       std::memory_order_release);
    s_tx_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

extern "C" bool nanojs8_cat_set_freq(uint64_t freq_hz) {
    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p || p->cat != NANOJS8_RADIO_CAT_CIV) {
        ESP_LOGI(TAG, "set_freq: skip (profile cat != CIV)");
        return false;
    }
    if (!p->can_set_freq) {
        ESP_LOGW(TAG, "set_freq: profile '%s' has can_set_freq=false",
                 p->id);
        return false;
    }
    uint8_t frame[11];
    size_t n = nanojs8_civ_build_set_freq(
        frame, sizeof(frame),
        p->cat_civ_radio_addr, p->cat_civ_ctrl_addr, freq_hz);
    if (n == 0) return false;

    ESP_LOGI(TAG, "CAT TX: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                  "(set freq %llu Hz)",
             frame[0], frame[1], frame[2], frame[3], frame[4],
             frame[5], frame[6], frame[7], frame[8], frame[9], frame[10],
             (unsigned long long)freq_hz);

    // L6b.6 build-fix2: stash the value BEFORE writing the frame so the
    // FB ack handler (which runs on the USB-host RX task and could fire
    // within ~10 ms) always sees the pending value. The radio's ack
    // carries no freq data of its own — we have to remember what we
    // sent in order to update s_last_freq_hz when the FB lands.
    s_pending_set_freq_hz.store(freq_hz, std::memory_order_release);

    esp_err_t err = nanojs8_serial_write(frame, n, 200);
    if (err != ESP_OK) {
        // Roll back the pending value — the radio never saw the set,
        // so any incoming FB must be for some other command.
        s_pending_set_freq_hz.store(0, std::memory_order_release);
        ESP_LOGW(TAG, "set_freq: serial_write failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    s_last_tx_ms.store((uint32_t)(esp_timer_get_time() / 1000),
                       std::memory_order_release);
    s_tx_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

extern "C" void nanojs8_cat_get_counters(uint32_t *frames_ok,
                                         uint32_t *frames_echoed,
                                         uint32_t *frames_dropped,
                                         uint32_t *tx_count) {
    // Parser counters live in the parser struct. These are written
    // only on the RX path, so reading them from the app loop without
    // a lock is racy but bounded — each is a 32-bit value, only ever
    // increments, and the worst case is an off-by-one in a heartbeat
    // log line. Acceptable for a diagnostic counter.
    if (frames_ok)      *frames_ok      = s_civ_rx.frames_ok;
    if (frames_echoed)  *frames_echoed  = s_civ_rx.frames_echoed;
    if (frames_dropped) *frames_dropped = s_civ_rx.frames_dropped;
    if (tx_count)       *tx_count       = s_tx_count.load(std::memory_order_relaxed);
}

extern "C" void nanojs8_cat_tick(void) {
    // L6b.6 fix2: tick has three jobs now —
    //   (1) fire the deferred initial probe queued by apply_profile,
    //   (2) fire the verify-read queued by the FB-ack handler,
    //   (3) periodically poll the radio to keep s_last_reply_ms fresh
    //       and to catch manual retunes on the radio's own dial.
    //
    // All three require the same gates: profile is CAT_CIV, serial is
    // READY, baud matches profile. Check once, then dispatch in
    // priority order (initial > verify > periodic).

    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p || p->cat != NANOJS8_RADIO_CAT_CIV) {
        // No CAT or non-CI-V profile — clear any leftover flags from a
        // prior profile and never poll. Polling a CAT-disabled radio
        // would just produce silence we'd misread as NO_REPLY.
        s_want_initial_probe.store(false, std::memory_order_release);
        s_want_verify_read.store(false, std::memory_order_release);
        return;
    }

    nanojs8_serial_info_t info = {};
    nanojs8_serial_get_info(&info);
    if (info.status != NANOJS8_SERIAL_STATUS_READY) {
        // Serial down — DigiRig unplugged or still enumerating. Bail
        // with deferred flags intact; we'll retry on the next tick.
        return;
    }

    // L6b.6: re-apply line config if needed (idempotent). Mostly
    // matters on first tick after boot when set_line during
    // apply_profile failed because serial wasn't enumerated yet.
    if (info.baud_rate != p->cat_baud) {
        esp_err_t err = nanojs8_serial_set_line(
            p->cat_baud, 8, NANOJS8_SERIAL_STOP_1, NANOJS8_SERIAL_PARITY_NONE);
        if (err != ESP_OK) {
            // Still not ready in a meaningful way. Leave flags set
            // and try again next tick.
            ESP_LOGD(TAG, "tick: serial_set_line not ready yet: %s",
                     esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "tick: deferred serial_set_line(%u 8N1) succeeded",
                 (unsigned)p->cat_baud);
    }

    // Priority 1: deferred initial probe. This is what fires the very
    // first read after a profile change once serial is up.
    if (s_want_initial_probe.exchange(false, std::memory_order_acq_rel)) {
        // Initial probe is a read — supersedes any queued verify-read
        // because the resulting RX response will refresh s_last_freq_hz
        // anyway. Clear verify too so we don't double-fire.
        s_want_verify_read.store(false, std::memory_order_release);
        ESP_LOGI(TAG, "tick: firing deferred initial freq probe");
        if (!nanojs8_cat_request_freq()) {
            ESP_LOGW(TAG, "tick: initial probe request_freq returned false");
        }
        return;
    }

    // Priority 2: verify-after-set read. Confirms the freq the radio
    // actually tuned to after a set_freq + FB ack.
    if (s_want_verify_read.exchange(false, std::memory_order_acq_rel)) {
        ESP_LOGI(TAG, "tick: firing verify-after-set read");
        if (!nanojs8_cat_request_freq()) {
            ESP_LOGW(TAG, "tick: verify-read request_freq returned false");
        }
        return;
    }

    // Priority 3: periodic poll. Fire CAT_POLL_INTERVAL_MS after the
    // last TX so we (a) keep s_last_reply_ms rolling — status stays OK
    // on HOME indefinitely as long as the radio responds — and (b)
    // pick up manual tuning the operator did on the radio's own dial.
    //
    // We measure interval from last_tx (not last_reply) so an
    // unresponsive radio still gets polled — if we waited on a reply
    // that never comes, we'd never retry. Quiet log only — periodic
    // polls would flood at INFO level. The tx/ok/echo/drop counters
    // in the 30 s heartbeat show poll health without per-poll noise.
    uint32_t now_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_tx = s_last_tx_ms.load(std::memory_order_acquire);
    bool poll_due = (last_tx == 0) ||
                    ((now_ms - last_tx) >= CAT_POLL_INTERVAL_MS);
    if (poll_due) {
        ESP_LOGD(TAG, "tick: periodic poll (%" PRIu32 " ms since last tx)",
                 (last_tx == 0) ? 0 : (now_ms - last_tx));
        if (!nanojs8_cat_request_freq()) {
            ESP_LOGD(TAG, "tick: periodic poll request_freq returned false");
        }
    }
}
