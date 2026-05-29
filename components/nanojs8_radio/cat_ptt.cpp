// NanoJS8 — (tr)uSDX CAT transport: CH34x VCP open + CAT PTT + status poll
//
// Analogous to cp2102_ptt.cpp (DigiRig RTS path), but for the (tr)uSDX:
//   - Opens the CH340 CDC port via the VCP umbrella (CH34x sub-driver).
//   - Binds the CAT command layer (cat_control.cpp) to that device.
//   - PTT is asserted via CAT (TX0;/RX;), NOT an RTS line.
//   - A slow-poll task queries IF/FA/MD every ~2 s so HOME shows live
//     frequency + mode.
//
// Why a separate file from cp2102_ptt.cpp: different chip (CH34x vs
// CP210x), different PTT mechanism (CAT command vs RTS line), and a
// background poll task the RTS path doesn't need. Keeping them separate
// keeps each path simple and independently testable.
//
// The (tr)uSDX presents as a SINGLE CDC device with no internal hub, so
// this path never touches ESP-IDF's experimental external-hub driver —
// the source of the assert crash on the DigiRig. This is the robust
// radio path on ESP32-S3.
//
// Safety: a software watchdog auto-releases PTT after the profile's
// ptt_max_hold_s, matching the RTS path and MicroJS8 behavior.

#include <atomic>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/cdc_acm_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"

#include "radio_profile.h"
#include "cat_control.h"

using esp_usb::VCP;
using esp_usb::CH34x;
// CdcAcmDevice is at global scope.

namespace nanojs8 {
namespace radio {

static const char* TAG = "radio_cat_ptt";

// ---------------------------------------------------------------------------
// State (singleton)
// ---------------------------------------------------------------------------

static std::atomic<bool>            s_installed{false};
static std::atomic<bool>            s_connected{false};
static std::atomic<bool>            s_ptt_active{false};
static std::atomic<int64_t>         s_ptt_on_since_us{0};
static std::unique_ptr<CdcAcmDevice> s_vcp{};
static const RadioProfile*          s_active_profile = nullptr;

static TaskHandle_t                 s_poll_task = nullptr;
static std::atomic<bool>            s_poll_run{false};

static int64_t now_us() { return esp_timer_get_time(); }

// ---------------------------------------------------------------------------
// CDC event callback (disconnect detection + diagnostic logging)
// ---------------------------------------------------------------------------
//
// The cdc_acm driver fires this for ALL device events, not just disconnect.
// For the Phase 3b spurious-disconnect investigation we log every event
// type, especially ERROR (which carries an esp_err_t in event->data.error
// telling us WHAT went wrong). The events arrive on the cdc_acm client
// task, not an ISR, so logging here is safe.

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t* event, void* arg) {
    (void)arg;
    if (!event) return;

    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            // This is the diagnostic we've been missing. event->data.error
            // is the esp_err_t reported by the underlying USB host stack.
            ESP_LOGE(TAG, "(tr)uSDX CH340 CDC ERROR: %s (0x%x)",
                     esp_err_to_name(event->data.error),
                     (unsigned)event->data.error);
            // ERROR doesn't necessarily mean disconnect; the stack may
            // recover. We DON'T tear down on ERROR alone — only on the
            // DISCONNECTED event below.
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            // UART line state notifications (DSR, RI, framing, parity,
            // overrun, break). If the CH340 reports a serial-state change
            // it may foreshadow a problem.
            ESP_LOGW(TAG, "(tr)uSDX CH340 serial-state change");
            break;

        case CDC_ACM_HOST_NETWORK_CONNECTION:
            ESP_LOGI(TAG, "(tr)uSDX CH340 network-connection event");
            break;

        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "(tr)uSDX CH340 disconnected");
            s_connected.store(false, std::memory_order_release);
            s_ptt_active.store(false, std::memory_order_release);
            cat::unbind();
            // The device object is invalidated by the stack; drop our
            // handle. The poll task will see !connected and idle.
            break;

        default:
            ESP_LOGW(TAG, "(tr)uSDX CH340 unknown event type %d", (int)event->type);
            break;
    }
}

// ---------------------------------------------------------------------------
// Slow-poll task — queries CAT status (~2 s) so HOME shows freq/mode.
// Also runs the PTT safety watchdog.
// ---------------------------------------------------------------------------

static void poll_task_entry(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "CAT poll task started on core %d", xPortGetCoreID());
    const uint16_t max_hold_s = s_active_profile ? s_active_profile->ptt_max_hold_s : 20;
    const int64_t  start_us   = now_us();

    int64_t last_poll_us      = 0;
    int64_t last_heartbeat_us = 0;
    uint32_t poll_count        = 0;
    uint32_t poll_err_count    = 0;

    while (s_poll_run.load(std::memory_order_acquire)) {
        const int64_t t = now_us();

        // Diagnostic heartbeat every 60s: uptime + poll stats so we can
        // correlate disconnect timing against what we were doing.
        if (t - last_heartbeat_us >= 60000000) {
            const uint32_t up_s = (uint32_t)((t - start_us) / 1000000);
            ESP_LOGI(TAG, "heartbeat: up=%us connected=%d polls=%u errs=%u",
                     up_s,
                     (int)s_connected.load(std::memory_order_acquire),
                     (unsigned)poll_count, (unsigned)poll_err_count);
            last_heartbeat_us = t;
        }

        // PTT safety watchdog: auto-release if held too long.
        if (s_ptt_active.load(std::memory_order_acquire)) {
            const int64_t held_s = (t - s_ptt_on_since_us.load(std::memory_order_acquire)) / 1000000;
            if (held_s >= (int64_t)max_hold_s) {
                ESP_LOGW(TAG, "PTT max-hold (%us) reached — auto-releasing", max_hold_s);
                cat::set_ptt(false);
                s_ptt_active.store(false, std::memory_order_release);
            }
        }

        // Status poll every ~30 s, but ONLY while not transmitting
        // (don't inject CAT queries mid-TX; the radio is streaming/keyed).
        //
        // Why 30 s (not 2 s): the (tr)uSDX is an Atmega328 running CAT
        // alongside DSP/audio/RF/display. Aggressive polling stresses its
        // firmware loop timing and eventually causes USB instability —
        // we observed clean disconnects after ~6 minutes of 2-second
        // polling in steady-state RX with no transmit. The (tr)uSDX
        // community independently arrived at the same conclusion:
        //   - N1UGK (FT8-over-CAT): "set the CAT polling to 30 seconds"
        //   - PE4BAS (WSJT-X with (tr)uSDX): 80 seconds
        // 30 s is the proven sweet spot — display lag of up to 30 s when
        // tuning on the rig's own knob is acceptable for an informational
        // display (the operator is looking at the rig when they turn it).
        if (s_connected.load(std::memory_order_acquire) &&
            !s_ptt_active.load(std::memory_order_acquire)) {
            if (t - last_poll_us >= 30000000) {
                const esp_err_t pe = cat::poll_status();
                ++poll_count;
                if (pe != ESP_OK) ++poll_err_count;
                last_poll_us = t;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "CAT poll task exiting");
    s_poll_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// VCP open (CH34x)
// ---------------------------------------------------------------------------

static bool try_open_vcp(uint32_t baud_rate) {
    cdc_acm_host_device_config_t dev_cfg = {};
    dev_cfg.connection_timeout_ms = 1000;
    dev_cfg.out_buffer_size       = 64;
    dev_cfg.in_buffer_size        = 64;                       // RX needed for CAT replies
    dev_cfg.event_cb              = cdc_event_cb;
    dev_cfg.data_cb               = cat::get_rx_callback();   // CAT reply bytes -> parser
    dev_cfg.user_arg              = nullptr;

    CdcAcmDevice* raw = VCP::open(&dev_cfg);
    if (!raw) {
        return false;  // no matching device on the bus right now
    }

    // Line coding: (tr)uSDX runs 115200 8N1 (firmware 2.00t+). Set it so
    // the CH340 clocks at the right rate.
    cdc_acm_line_coding_t lc = {};
    lc.dwDTERate   = baud_rate;
    lc.bCharFormat = 0;  // 1 stop bit
    lc.bParityType = 0;  // none
    lc.bDataBits   = 8;
    esp_err_t err = raw->line_coding_set(&lc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "line_coding_set failed: %s — continuing", esp_err_to_name(err));
    }

    // (tr)uSDX CAT spec: DTR HIGH, RTS LOW on RX. Set DTR=true, RTS=false.
    err = raw->set_control_line_state(true /*dtr*/, false /*rts*/);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_control_line_state(DTR=1,RTS=0) failed: %s — continuing",
                 esp_err_to_name(err));
    }

    s_vcp.reset(raw);

    // Bind CAT to this device and do the ID; handshake.
    const esp_err_t bind_err = cat::bind(s_vcp.get());
    if (bind_err == ESP_OK) {
        ESP_LOGI(TAG, "(tr)uSDX CAT bound — CH340 @ %u baud, DTR high, RTS low",
                 (unsigned)baud_rate);
    } else {
        // Non-fatal: we still mark connected so PTT can be attempted.
        // bind() already logged the specifics.
        ESP_LOGW(TAG, "(tr)uSDX CAT handshake imperfect (%s) — proceeding",
                 esp_err_to_name(bind_err));
    }

    s_connected.store(true, std::memory_order_release);
    s_ptt_active.store(false, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Public interface (mirrors cp2102_ptt_* so radio_service can dispatch)
// ---------------------------------------------------------------------------

esp_err_t cat_ptt_start(const RadioProfile* profile) {
    if (s_installed.exchange(true)) {
        return ESP_OK;  // idempotent
    }
    s_active_profile = profile;

    // Register the CH34x driver with the VCP umbrella. The CP210x driver
    // may already be registered (DigiRig path); the umbrella supports
    // multiple registered drivers and auto-selects by VID:PID, so
    // registering CH34x here is additive and safe.
    VCP::register_driver<CH34x>();

    // Open attempt is retried by the radio service's enumeration flow;
    // here we just try once. If the device isn't present yet, we stay
    // "installed but not connected" and a later open attempt (triggered
    // by the USB enumeration callback path) will succeed.
    const uint32_t baud = profile ? profile->baud_rate : 115200;
    try_open_vcp(baud);

    // Start the slow-poll task regardless of immediate connect; it idles
    // until connected.
    s_poll_run.store(true, std::memory_order_release);
    xTaskCreatePinnedToCore(poll_task_entry, "radio_cat_poll",
                            4096, nullptr, 4, &s_poll_task, 1);

    return ESP_OK;
}

void cat_ptt_stop(void) {
    if (!s_installed.exchange(false)) {
        return;
    }
    // Release PTT if asserted, before tearing down.
    if (s_ptt_active.load(std::memory_order_acquire)) {
        cat::set_ptt(false);
        s_ptt_active.store(false, std::memory_order_release);
    }

    // Stop the poll task and wait briefly for it to exit.
    s_poll_run.store(false, std::memory_order_release);
    for (int i = 0; i < 20 && s_poll_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    cat::unbind();
    s_connected.store(false, std::memory_order_release);
    s_vcp.reset();   // closes the CDC device
    s_active_profile = nullptr;
}

bool cat_ptt_is_connected(void) {
    return s_connected.load(std::memory_order_acquire);
}

bool cat_ptt_is_active(void) {
    return s_ptt_active.load(std::memory_order_acquire);
}

esp_err_t cat_ptt_assert(void) {
    if (!s_connected.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = cat::set_ptt(true);
    if (err == ESP_OK) {
        s_ptt_active.store(true, std::memory_order_release);
        s_ptt_on_since_us.store(now_us(), std::memory_order_release);
    }
    return err;
}

esp_err_t cat_ptt_release(void) {
    if (!s_connected.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = cat::set_ptt(false);
    if (err == ESP_OK) {
        s_ptt_active.store(false, std::memory_order_release);
    }
    return err;
}

} // namespace radio
} // namespace nanojs8
