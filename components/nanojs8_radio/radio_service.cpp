// NanoJS8 — Radio service orchestrator
//
// Implements the public API declared in include/radio_service.h.
// All cross-component coordination lives here:
//
//   start():
//     - lookup the active RadioProfile by NVS id
//     - install usb_host library (usb_host_task.cpp)
//     - install cdc_acm host (singleton, for VCP underpinnings)
//     - install uac_manager (uac_manager.cpp) — class driver task
//     - start cp2102_ptt (cp2102_ptt.cpp)         — open task
//
//   stop():
//     - request all subsystems to stop
//     - wait briefly for them to settle
//     - they tear down themselves on their own tasks
//
//   snapshot():
//     - assembles a coherent struct from the underlying state, with
//       a status_text string ready for HOME rendering
//
//   ptt_on/ptt_off:
//     - delegates to cp2102_ptt for profiles using RTS PTT
//     - returns ESP_ERR_NOT_SUPPORTED for profiles with PttMethod::CAT
//       (Phase 3c will add the CAT path)
//
// Lifetime constraints:
//   - usb_host_start() must complete BEFORE uac_host_install or
//     cdc_acm_host_install (per IDF docs)
//   - cdc_acm_host_install() must complete BEFORE VCP::register_driver
//     and VCP::open() in cp2102_ptt_start()
//
// The orchestrator enforces this serialization.

#include <atomic>
#include <cstring>
#include <cstdio>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/cdc_acm_host.h"

#include "radio_service.h"
#include "radio_profile.h"
#include "radio_rx_buffer.h"
#include "cat_control.h"

#include "config_store.h"

namespace nanojs8 {
namespace radio {

// ---------------------------------------------------------------------------
// Cross-file forwards (these are implemented in the sibling .cpp files
// within this component; not part of the public radio_service.h API)
// ---------------------------------------------------------------------------

extern esp_err_t usb_host_start(void);
extern void      usb_host_stop(void);
extern bool      usb_host_is_installed(void);

extern esp_err_t uac_manager_start(const RadioProfile* profile);
extern void      uac_manager_stop(void);
extern bool      uac_manager_is_streaming(void);
extern size_t    uac_manager_tx_write(const int16_t* src, size_t sample_count);

extern esp_err_t cp2102_ptt_start(const RadioProfile* profile);
extern void      cp2102_ptt_stop(void);
extern bool      cp2102_ptt_is_connected(void);
extern bool      cp2102_ptt_is_active(void);
extern esp_err_t cp2102_ptt_assert(void);
extern esp_err_t cp2102_ptt_release(void);

// (tr)uSDX CAT path (cat_ptt.cpp). Parallel to the cp2102_* RTS path
// but uses CAT commands for PTT and runs a status-poll task.
extern esp_err_t cat_ptt_start(const RadioProfile* profile);
extern void      cat_ptt_stop(void);
extern bool      cat_ptt_is_connected(void);
extern bool      cat_ptt_is_active(void);
extern esp_err_t cat_ptt_assert(void);
extern esp_err_t cat_ptt_release(void);

// ---------------------------------------------------------------------------
// Service state
// ---------------------------------------------------------------------------

static const char* TAG = "radio_svc";

static std::atomic<Status>         s_status{Status::IDLE};
static std::atomic<bool>           s_cdc_installed{false};
static const RadioProfile*         s_active_profile = nullptr;
static std::atomic<uint32_t>       s_enum_attempts{0};
static std::atomic<uint32_t>       s_last_event_ms{0};

// ---------------------------------------------------------------------------
// Status string builder
// ---------------------------------------------------------------------------
//
// Returns a short text suitable for the HOME screen's CAT row. The
// exact wording matches what the operator expects from MicroJS8's
// status indicators where possible.
static const char* status_text_for(Status s) {
    switch (s) {
        case Status::IDLE:        return "Disconnected";
        case Status::ENUMERATING: return "Waiting...";
        case Status::CONNECTED:   return "Connected";
        case Status::ERROR:       return "Error";
    }
    return "Unknown";
}

static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

esp_err_t start(void) {
    if (s_status.load(std::memory_order_acquire) != Status::IDLE) {
        ESP_LOGI(TAG, "start: already running");
        return ESP_OK;
    }

    // Resolve the active profile from NVS.
    const Config& cfg = nanojs8::config::current();
    const RadioProfile* prof = lookup_by_id(cfg.radio);
    if (!prof) {
        ESP_LOGE(TAG, "start: unknown radio profile '%s' in NVS — "
                      "set RADIO in SETUP first", cfg.radio);
        return ESP_ERR_INVALID_STATE;
    }
    if (!prof->is_supported_now) {
        ESP_LOGW(TAG, "start: profile '%s' is not supported in this firmware "
                      "phase. Choose 'digirig_unknown' on the SETUP screen.",
                 prof->id);
        return ESP_ERR_INVALID_STATE;
    }
    s_active_profile = prof;
    ESP_LOGI(TAG, "Starting radio service for profile '%s' (%s)",
             prof->id, prof->display_name);

    // Tame USB-stack log spam. ESP-IDF's external-hub driver is
    // experimental in v5.5.x and logs "Hub status change has not been
    // implemented yet" at WARN level — on every hub status-change poll.
    // The DigiRig contains an INTERNAL USB hub (CM108 + CP2102 behind a
    // hub), so this fires constantly, especially under marginal bus
    // power, flooding the console and stealing CPU from enumeration.
    //
    // We can't fix the driver (it's in the IDF tree, not our vendored
    // copy), but we can mute its noise so real diagnostics are visible
    // and the logging itself stops consuming cycles. We raise the
    // threshold for the noisy tags to ERROR so genuine errors still
    // show but the WARN spam is dropped.
    //
    // These calls are idempotent and cheap; re-applying on every start()
    // is harmless and survives any IDF re-init that might reset levels.
    esp_log_level_set("EXT_HUB", ESP_LOG_ERROR);
    esp_log_level_set("EXT_PORT", ESP_LOG_ERROR);

    // Phase 3b diagnostic: spurious (tr)uSDX CH340 disconnects of unknown
    // cause. Bump the USB-host stack and cdc_acm to DEBUG so we capture
    // whatever event sequence precedes the disconnect — endpoint stalls,
    // transfer errors, descriptor failures, or just a clean device-gone
    // notification. DEBUG (not VERBOSE) is enough; VERBOSE produces a
    // flood without proportional diagnostic value. The default INFO level
    // showed nothing prior to the disconnect, which is itself a clue —
    // the cdc_acm callback fires WITHOUT preceding error logs at INFO,
    // so the actual cause is hiding at WARN/DEBUG. This block can be
    // pulled back to INFO once the cause is identified.
    esp_log_level_set("USBH",     ESP_LOG_DEBUG);
    esp_log_level_set("HCD",      ESP_LOG_DEBUG);
    esp_log_level_set("ENUM",     ESP_LOG_DEBUG);
    esp_log_level_set("cdc_acm",  ESP_LOG_DEBUG);

    // Pre-flight: ensure the RX ring is ready. uac_manager will also
    // call rx_buffer_init() but doing it here guarantees the buffer
    // exists before any other subsystem races a write into it.
    if (!rx_buffer_init()) {
        ESP_LOGE(TAG, "rx_buffer_init failed");
        return ESP_ERR_NO_MEM;
    }

    // 1. USB host library. This installs the host stack and starts
    //    its event-pump task. Blocks up to 5 s.
    esp_err_t err = usb_host_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_start failed: %s", esp_err_to_name(err));
        s_status.store(Status::ERROR, std::memory_order_release);
        return err;
    }

    // 2. CDC-ACM host driver — needed underneath the VCP service.
    //    Singleton: install once per boot.
    if (!s_cdc_installed.load(std::memory_order_acquire)) {
        const cdc_acm_host_driver_config_t cdc_cfg = {
            .driver_task_stack_size = 4096,
            .driver_task_priority   = 4,
            .xCoreID                = 1,
            .new_dev_cb             = nullptr,
        };
        err = cdc_acm_host_install(&cdc_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cdc_acm_host_install failed: %s",
                     esp_err_to_name(err));
            usb_host_stop();
            s_status.store(Status::ERROR, std::memory_order_release);
            return err;
        }
        s_cdc_installed.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "CDC-ACM host driver installed");
    }

    // 3 & 4. Branch by PTT method.
    //
    // RTS profiles (DigiRig): start the UAC audio class driver AND the
    // CP2102 RTS-PTT path.
    //
    // CAT profiles ((tr)uSDX, step-1): CAT control only — no UAC audio
    // yet (audio-over-CAT is Phase 3b-step-2). Start just the CAT path.
    // This also means the (tr)uSDX, a single CDC device with no internal
    // hub, never instantiates the UAC class driver or touches the audio
    // isochronous pipes — keeping its footprint to ~3 HCD channels.
    if (prof->ptt_method == PttMethod::CAT) {
        err = cat_ptt_start(prof);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cat_ptt_start failed: %s", esp_err_to_name(err));
            s_status.store(Status::ERROR, std::memory_order_release);
            return err;
        }
    } else {
        // 3. UAC class driver task (RX_CONNECTED when CM108 enumerates).
        err = uac_manager_start(prof);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uac_manager_start failed: %s", esp_err_to_name(err));
            s_status.store(Status::ERROR, std::memory_order_release);
            return err;
        }

        // 4. CP2102 RTS-PTT task. Retries open until DigiRig serial enumerates.
        err = cp2102_ptt_start(prof);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cp2102_ptt_start failed: %s", esp_err_to_name(err));
            uac_manager_stop();
            s_status.store(Status::ERROR, std::memory_order_release);
            return err;
        }
    }

    s_status.store(Status::ENUMERATING, std::memory_order_release);
    s_last_event_ms.store(now_ms(), std::memory_order_release);
    ESP_LOGI(TAG, "Radio service started; waiting for %s enumeration",
             prof->ptt_method == PttMethod::CAT ? "(tr)uSDX CH340" : "DigiRig");
    return ESP_OK;
}

esp_err_t stop(void) {
    if (s_status.load(std::memory_order_acquire) == Status::IDLE) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Stopping radio service");

    // Order of teardown matters and is the reverse of start():
    //   1. cp2102_ptt_stop  — closes the VCP device handle, exits its task
    //   2. uac_manager_stop — closes the UAC device handle, uninstalls
    //                         the UAC class driver (deregisters as USB client)
    //   3. cdc_acm_host_uninstall — deregisters CDC-ACM as USB client
    //                         (must happen BEFORE usb_host_stop so the
    //                          host lib sees ALL_FREE and can uninstall)
    //   4. usb_host_stop    — drains events, calls usb_host_uninstall,
    //                         releases the PHY and the controller
    // Stop whichever PTT path is active. Both are idempotent and safe
    // to call even if not started, so we can call both without tracking
    // which profile was active.
    cp2102_ptt_stop();
    cat_ptt_stop();
    uac_manager_stop();

    // Brief delay to let the per-client tasks notice the stop request,
    // close their handles, and self-delete. They're cooperative.
    vTaskDelay(pdMS_TO_TICKS(500));

    // Now CDC-ACM has no devices open; we can uninstall its class driver.
    // This is the missing step that left ESP_ERR_INVALID_STATE on the
    // subsequent usb_host_install() in v0.3.0 initial.
    if (s_cdc_installed.load(std::memory_order_acquire)) {
        const esp_err_t err = cdc_acm_host_uninstall();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CDC-ACM host driver uninstalled");
            s_cdc_installed.store(false, std::memory_order_release);
        } else {
            // ESP_ERR_NOT_FINISHED here means there's still an open
            // CdcAcmDevice somewhere. We logged it but proceed — the
            // host lib uninstall below will report whether everything
            // actually unwound.
            ESP_LOGW(TAG, "cdc_acm_host_uninstall: %s — continuing",
                     esp_err_to_name(err));
        }
    }

    // Now request the host library to tear down. Its task will drain
    // any remaining events, see ALL_FREE (because we just uninstalled
    // all clients), and successfully call usb_host_uninstall().
    usb_host_stop();

    // Give the host task time to actually run its uninstall path before
    // we return. Without this, a quick stop()→start() race would re-call
    // usb_host_install() before the previous task finished uninstalling.
    vTaskDelay(pdMS_TO_TICKS(300));

    s_status.store(Status::IDLE, std::memory_order_release);
    s_active_profile = nullptr;
    s_last_event_ms.store(now_ms(), std::memory_order_release);
    ESP_LOGI(TAG, "Radio service stopped");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// State observation
// ---------------------------------------------------------------------------

Status status(void) {
    return s_status.load(std::memory_order_acquire);
}

void snapshot(Snapshot* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));

    // Refresh status based on subsystem health if we're currently
    // started. The "connected" criterion differs by PTT method:
    //   RTS (DigiRig): UAC streaming AND CP2102 serial both up.
    //   CAT ((tr)uSDX): the single CH340 CDC port is open (no UAC).
    const bool is_cat = s_active_profile &&
                        s_active_profile->ptt_method == PttMethod::CAT;

    Status s = s_status.load(std::memory_order_acquire);
    if (s != Status::IDLE && s != Status::ERROR) {
        bool connected_now;
        if (is_cat) {
            connected_now = cat_ptt_is_connected();
        } else {
            connected_now = uac_manager_is_streaming() && cp2102_ptt_is_connected();
        }
        if (connected_now) {
            s = Status::CONNECTED;
        } else if (s == Status::CONNECTED) {
            s = Status::ENUMERATING;
        }
        s_status.store(s, std::memory_order_release);
    }

    out->status     = s;
    out->ptt_active = is_cat ? cat_ptt_is_active() : cp2102_ptt_is_active();

    if (s_active_profile) {
        std::strncpy(out->profile_id,   s_active_profile->id,
                     sizeof(out->profile_id) - 1);
        std::strncpy(out->display_name, s_active_profile->display_name,
                     sizeof(out->display_name) - 1);
        out->supports_freq = s_active_profile->cat_provides_freq;
    } else {
        std::strncpy(out->profile_id,   "(none)", sizeof(out->profile_id) - 1);
        std::strncpy(out->display_name, "(none)", sizeof(out->display_name) - 1);
        out->supports_freq = false;
    }

    // Status text: show the profile name when connected, else the
    // generic phase-of-life string.
    if (s == Status::CONNECTED && s_active_profile) {
        std::snprintf(out->status_text, sizeof(out->status_text),
                      "%s%s", s_active_profile->display_name,
                      out->ptt_active ? " *TX*" : "");
    } else {
        std::strncpy(out->status_text, status_text_for(s),
                     sizeof(out->status_text) - 1);
    }

    // Frequency: CAT profiles read live VFO from the radio. RTS profiles
    // (DigiRig) have no CAT, so freq stays 0.
    if (is_cat) {
        cat::CatState cs;
        cat::get_state(&cs);
        out->freq_hz = cs.freq_hz;
    } else {
        out->freq_hz = 0;
    }
    out->rx_frames_total = rx_buffer_frames_total();
    out->rx_overruns     = rx_buffer_overrun_count();
    out->enum_attempts   = s_enum_attempts.load(std::memory_order_acquire);
    out->last_event_ms   = s_last_event_ms.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// PTT
// ---------------------------------------------------------------------------

esp_err_t ptt_on(void) {
    if (!s_active_profile) return ESP_ERR_INVALID_STATE;
    const bool is_cat = s_active_profile->ptt_method == PttMethod::CAT;

    // PTT can be asserted as soon as the relevant transport is connected,
    // even if (for RTS profiles) UAC is still warming up.
    const bool transport_up = is_cat ? cat_ptt_is_connected()
                                     : cp2102_ptt_is_connected();
    if (!transport_up) {
        return ESP_ERR_INVALID_STATE;
    }
    switch (s_active_profile->ptt_method) {
        case PttMethod::RTS:  return cp2102_ptt_assert();
        case PttMethod::CAT:  return cat_ptt_assert();
        case PttMethod::NONE: return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t ptt_off(void) {
    if (!s_active_profile) return ESP_ERR_INVALID_STATE;
    const bool is_cat = s_active_profile->ptt_method == PttMethod::CAT;
    const bool transport_up = is_cat ? cat_ptt_is_connected()
                                     : cp2102_ptt_is_connected();
    if (!transport_up) {
        return ESP_ERR_INVALID_STATE;
    }
    switch (s_active_profile->ptt_method) {
        case PttMethod::RTS:  return cp2102_ptt_release();
        case PttMethod::CAT:  return cat_ptt_release();
        case PttMethod::NONE: return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_INVALID_STATE;
}

// ---------------------------------------------------------------------------
// RX / TX accessors
// ---------------------------------------------------------------------------

size_t rx_read(int16_t* dest, size_t max_samples) {
    return rx_buffer_read(dest, max_samples);
}

void rx_drain(void) {
    rx_buffer_drain();
}

size_t tx_write(const int16_t* src, size_t sample_count) {
    return uac_manager_tx_write(src, sample_count);
}

} // namespace radio
} // namespace nanojs8
