// NanoJS8 — CP2102 RTS-based PTT
//
// Drives the PTT line on DigiRig-style interfaces, which route the
// CP2102 RTS pin to the radio's PTT input via an optocoupler.
//
// Why a separate file from cdc_acm: CP2102 (and CP2102N) do NOT
// implement the standard CDC-ACM SET_CONTROL_LINE_STATE control
// transfer. Instead they expose a vendor request CP210X_SET_MHS
// (request 0x07, wValue encodes DTR/RTS with mask bits).
//
// The Espressif `usb_host_vcp` umbrella + `usb_host_cp210x_vcp`
// sub-driver wraps this for us: VCP::open() returns a CdcAcmDevice*
// whose set_control_line_state(dtr, rts) internally issues the
// correct vendor request when the underlying device is a CP210x.
//
// Phase 3a only needs RTS toggling (PTT) and one initial
// line_coding_set() to satisfy the chip's firmware (some CP2102
// rev firmware refuses subsequent control transfers until a baud
// rate has been set at least once).
//
// Safety: a software watchdog auto-releases PTT after the profile's
// ptt_max_hold_s expires. This matches MicroJS8's safety behavior —
// if the modulator hangs in TX state we don't transmit indefinitely.

#include <atomic>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/cdc_acm_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_cp210x.hpp"

#include "radio_profile.h"

using esp_usb::VCP;
using esp_usb::CP210x;
// CdcAcmDevice is at global scope (after the extern "C" block in
// cdc_acm_host.h closes), so no using-declaration needed.

namespace nanojs8 {
namespace radio {

static const char* TAG = "radio_ptt";

// ---------------------------------------------------------------------------
// State (singleton)
// ---------------------------------------------------------------------------

static std::atomic<bool>           s_installed{false};
static std::atomic<bool>           s_connected{false};
static std::atomic<bool>           s_ptt_active{false};
static std::atomic<int64_t>        s_ptt_on_since_us{0};
static std::unique_ptr<CdcAcmDevice> s_vcp{};
static const RadioProfile*         s_active_profile = nullptr;

// Background task that retries VCP::open() until it succeeds and then
// runs the PTT-hold watchdog while connected. Recreated on every
// service start; exits when service stops or VCP disconnects.
static TaskHandle_t                s_task = nullptr;
static std::atomic<bool>           s_stop_requested{false};

// ---------------------------------------------------------------------------
// CDC event callback — handles disconnect
// ---------------------------------------------------------------------------

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t* event,
                         void* user_ctx) {
    (void)user_ctx;
    if (!event) return;
    switch (event->type) {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "CP2102 disconnected");
            s_connected.store(false, std::memory_order_release);
            // Clear PTT state immediately — the radio has lost USB
            // anyway, but state should reflect reality so HOME shows
            // it correctly and ptt_on() returns ESP_ERR_INVALID_STATE
            // until reconnect.
            s_ptt_active.store(false, std::memory_order_release);
            // Drop the VCP handle when safe to do so. Note: this
            // callback fires from the CDC-ACM driver task; deleting
            // the unique_ptr is normally safe but we'd rather let the
            // background task notice the disconnect and clean up. So
            // here we just flip the flag.
            break;
        case CDC_ACM_HOST_ERROR:
            ESP_LOGW(TAG, "CP2102 error: %d", event->data.error);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Try to open the VCP device — non-blocking; called periodically
// from the background task while waiting for the CP2102 to appear.
// ---------------------------------------------------------------------------

static bool try_open_vcp(uint32_t baud_rate) {
    cdc_acm_host_device_config_t dev_cfg = {};
    dev_cfg.connection_timeout_ms = 1000;   // VCP::open's per-attempt timeout
    dev_cfg.out_buffer_size       = 64;
    dev_cfg.in_buffer_size        = 0;       // RX not used in Phase 3a
    dev_cfg.event_cb              = cdc_event_cb;
    dev_cfg.data_cb               = nullptr;
    dev_cfg.user_arg              = nullptr;

    CdcAcmDevice* raw = VCP::open(&dev_cfg);
    if (!raw) {
        return false;  // no matching device on the bus right now
    }

    // Set line coding. Required by some CP2102 firmware before
    // subsequent control transfers are accepted. PTT-only operation
    // doesn't actually use the UART data path but the chip's state
    // machine wants the parameters anyway.
    cdc_acm_line_coding_t lc = {};
    lc.dwDTERate  = baud_rate;
    lc.bCharFormat = 0;  // 1 stop bit
    lc.bParityType = 0;  // none
    lc.bDataBits   = 8;
    esp_err_t err = raw->line_coding_set(&lc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "line_coding_set failed: %s — continuing anyway",
                 esp_err_to_name(err));
        // We don't abort: some CP2102N variants reject line_coding_set
        // but still accept set_control_line_state.
    }

    // Initialize: RTS off (PTT released), DTR off.
    err = raw->set_control_line_state(false, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial set_control_line_state failed: %s",
                 esp_err_to_name(err));
        delete raw;
        return false;
    }

    s_vcp.reset(raw);
    s_connected.store(true, std::memory_order_release);
    s_ptt_active.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "CP2102 VCP opened — line coding %lu baud, RTS released",
             (unsigned long)baud_rate);
    return true;
}

// ---------------------------------------------------------------------------
// Background task — open retry loop + PTT-hold watchdog
// ---------------------------------------------------------------------------

static void ptt_task_entry(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "PTT task started on core %d", xPortGetCoreID());

    constexpr uint32_t SCAN_INTERVAL_MS = 500;
    constexpr uint32_t WATCHDOG_TICK_MS = 100;

    while (!s_stop_requested.load(std::memory_order_acquire)) {
        if (!s_connected.load(std::memory_order_acquire)) {
            // Tear down stale handle if disconnect callback flipped
            // the flag while we held a handle.
            if (s_vcp) {
                ESP_LOGI(TAG, "Releasing stale VCP handle");
                s_vcp.reset();
            }
            // Try to open once per SCAN_INTERVAL_MS.
            if (s_active_profile) {
                if (try_open_vcp(s_active_profile->baud_rate)) {
                    // Connected — fall through to watchdog loop on next iter.
                }
            }
            vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
            continue;
        }

        // Connected — run watchdog. If PTT has been on longer than the
        // profile's ptt_max_hold_s, force-release it.
        if (s_ptt_active.load(std::memory_order_acquire) && s_active_profile) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t on_us  = s_ptt_on_since_us.load(std::memory_order_acquire);
            const int64_t hold_us = (int64_t)s_active_profile->ptt_max_hold_s * 1000000;
            if (on_us > 0 && (now_us - on_us) >= hold_us) {
                ESP_LOGW(TAG, "PTT max-hold reached (%us); auto-releasing",
                         (unsigned)s_active_profile->ptt_max_hold_s);
                if (s_vcp) {
                    esp_err_t err = s_vcp->set_control_line_state(false, false);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Auto-release set_control_line_state failed: %s",
                                 esp_err_to_name(err));
                    }
                }
                s_ptt_active.store(false, std::memory_order_release);
                s_ptt_on_since_us.store(0, std::memory_order_release);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_TICK_MS));
    }

    // Shutdown: release PTT, close VCP.
    if (s_vcp) {
        if (s_ptt_active.load(std::memory_order_acquire)) {
            s_vcp->set_control_line_state(false, false);
        }
        s_vcp.reset();
    }
    s_connected.store(false, std::memory_order_release);
    s_ptt_active.store(false, std::memory_order_release);
    s_task = nullptr;
    ESP_LOGI(TAG, "PTT task exiting");
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API used by radio_service.cpp
// ---------------------------------------------------------------------------

esp_err_t cp2102_ptt_start(const RadioProfile* profile) {
    if (!profile) return ESP_ERR_INVALID_ARG;

    if (s_installed.load(std::memory_order_acquire)) {
        return ESP_OK;  // idempotent
    }

    // The cdc_acm_host driver itself must be installed by the
    // orchestrator (radio_service.cpp) BEFORE this start() call,
    // because cdc_acm_host_install() is a singleton and other client
    // drivers (Phase 3b/c) may also rely on it.

    // Register CP210x driver into the VCP service. Safe to call more
    // than once — register_driver is idempotent on already-registered
    // driver types.
    VCP::register_driver<CP210x>();
    // Phase 3b/c will additionally register CH34x and FTDI here.

    s_active_profile = profile;
    s_stop_requested.store(false, std::memory_order_release);
    s_connected.store(false, std::memory_order_release);
    s_ptt_active.store(false, std::memory_order_release);
    s_ptt_on_since_us.store(0, std::memory_order_release);

    BaseType_t ok = xTaskCreatePinnedToCore(
        ptt_task_entry, "radio_ptt",
        4096, nullptr,
        4 /* prio */, &s_task, 1 /* core 1 */);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "PTT task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_installed.store(true, std::memory_order_release);
    return ESP_OK;
}

void cp2102_ptt_stop(void) {
    if (!s_installed.load(std::memory_order_acquire)) return;
    s_stop_requested.store(true, std::memory_order_release);
    s_installed.store(false, std::memory_order_release);
    // Background task will exit on its own; radio_service::stop() does
    // a small post-delay before declaring stop complete.
}

// Returns true if the CP2102 is currently enumerated and ready.
bool cp2102_ptt_is_connected(void) {
    return s_connected.load(std::memory_order_acquire);
}

bool cp2102_ptt_is_active(void) {
    return s_ptt_active.load(std::memory_order_acquire);
}

esp_err_t cp2102_ptt_assert(void) {
    if (!s_connected.load(std::memory_order_acquire) || !s_vcp) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_active_profile) {
        return ESP_ERR_INVALID_STATE;
    }
    // Assert RTS, keep DTR low. DigiRig's PTT optocoupler is wired to
    // RTS only — DTR is unused.
    esp_err_t err = s_vcp->set_control_line_state(false, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ptt_assert: set_control_line_state failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_ptt_active.store(true, std::memory_order_release);
    s_ptt_on_since_us.store(esp_timer_get_time(), std::memory_order_release);

    // Profile-mandated PTT-on delay: gives the radio time to settle in
    // TX before audio is sent. Phase 5 modulator should NOT start
    // sending samples until this delay has elapsed. Phase 3a's manual
    // `ptt on` command doesn't follow with audio, so the delay is
    // effectively cosmetic here, but we still honor it for parity with
    // Phase 5's contract.
    if (s_active_profile->ptt_on_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_active_profile->ptt_on_delay_ms));
    }
    ESP_LOGI(TAG, "PTT asserted (RTS high)");
    return ESP_OK;
}

esp_err_t cp2102_ptt_release(void) {
    if (!s_connected.load(std::memory_order_acquire) || !s_vcp) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_active_profile) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_vcp->set_control_line_state(false, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ptt_release: set_control_line_state failed: %s",
                 esp_err_to_name(err));
        // Even on transport error, treat as released — better safe than
        // stuck-on. The next plug cycle will resync state.
    }
    s_ptt_active.store(false, std::memory_order_release);
    s_ptt_on_since_us.store(0, std::memory_order_release);

    if (s_active_profile->ptt_off_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_active_profile->ptt_off_delay_ms));
    }
    ESP_LOGI(TAG, "PTT released (RTS low)");
    return err;
}

} // namespace radio
} // namespace nanojs8
