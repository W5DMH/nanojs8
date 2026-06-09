/*
 * usb_serial.cpp — NanoJS8 v0.7 USB serial implementation
 * =========================================================
 * See usb_serial.h for the public API and threading model.
 *
 * Architecture:
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  CDC-ACM Host driver  (installed via cdc_acm_host_install)  │
 *   │  Runs its own internal task, fires our event_cb and data_cb │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │  Open-loop task       (our code — calls VCP::open() until   │
 *   │                        a CP2102 enumerates, then sits in    │
 *   │                        xSemaphoreTake until disconnect)     │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Notable design points:
 *
 *   - The cdc_acm_host driver and the usb_host_uac driver coexist on the
 *     same USB host library installed by audio.cpp. We do NOT install the
 *     USB host stack ourselves — audio.cpp does that and we rely on it.
 *
 *   - VCP::open() blocks until either a recognized VCP device shows up or
 *     the connection_timeout_ms expires. We loop with a 5-second window so
 *     hot-plug works: plug in DigiRig later and it gets picked up.
 *
 *   - The CdcAcmDevice handle is stored as a raw pointer in s_device. The
 *     unique_ptr from VCP::open() lives in the open-loop task; on
 *     disconnect (event_cb fires DEVICE_DISCONNECTED) we release the
 *     unique_ptr, which closes the device. Public API calls check
 *     s_device for null and refuse to act if it is.
 *
 *   - We protect access to the CdcAcmDevice methods with s_device_mutex.
 *     The cdc_acm_host API is thread-safe internally but our `set_line`
 *     and `write` calls happen from the app's main loop while the open-
 *     loop task may be tearing down s_device after a disconnect. The
 *     mutex prevents the use-after-free.
 *
 * License: GPL-3.0
 */

#include "usb_serial.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "usb/cdc_acm_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_cp210x.hpp"

#include <atomic>
#include <memory>
#include <cstring>

using esp_usb::VCP;
using esp_usb::CP210x;
// Note: CdcAcmDevice is at global scope, NOT inside esp_usb::

static const char* TAG = "serial";

// ---------------------------------------------------------------------------
// Module-local state
// ---------------------------------------------------------------------------

namespace {

constexpr int OPEN_TASK_PRIORITY = 4;
constexpr int OPEN_TASK_STACK    = 6144;   // C++/STL handle + control xfers

// All access to s_device goes through s_device_mutex. The mutex is taken
// for the duration of any CdcAcmDevice method call, plus the open/close
// transitions in the open-loop task. NOT held across the long
// xSemaphoreTake(s_disconnect_sem) — that would deadlock the event_cb.
SemaphoreHandle_t s_device_mutex = nullptr;
CdcAcmDevice*     s_device = nullptr;   // raw — owned by the open-loop task

// Signaled by event_cb when DEVICE_DISCONNECTED fires
SemaphoreHandle_t s_disconnect_sem = nullptr;

// State exposed via nanojs8_serial_get_info()
std::atomic<nanojs8_serial_status_t> s_status{NANOJS8_SERIAL_STATUS_UNAVAILABLE};
std::atomic<uint32_t> s_baud{NANOJS8_SERIAL_DEFAULT_BAUD};
std::atomic<uint8_t>  s_data_bits{8};
std::atomic<uint8_t>  s_stop_bits{NANOJS8_SERIAL_STOP_1};
std::atomic<uint8_t>  s_parity{NANOJS8_SERIAL_PARITY_NONE};
std::atomic<bool>     s_dtr{false};
std::atomic<bool>     s_rts{false};
std::atomic<nanojs8_serial_ptt_line_t> s_ptt_line{NANOJS8_SERIAL_PTT_RTS};
std::atomic<uint16_t> s_vid{0};
std::atomic<uint16_t> s_pid{0};
std::atomic<uint64_t> s_tx_bytes{0};
std::atomic<uint64_t> s_rx_bytes{0};

// RX callback registered by app code. Read by the CDC driver's task.
std::atomic<nanojs8_serial_rx_cb_t> s_rx_cb{nullptr};
std::atomic<void*>                   s_rx_cb_arg{nullptr};

bool s_started = false;

} // anonymous namespace

// ---------------------------------------------------------------------------
// CDC-ACM event/data callbacks (called from CDC driver's task)
// ---------------------------------------------------------------------------

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t* event,
                          void* /*arg*/) {
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "CDC error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Device disconnected");
        s_status.store(NANOJS8_SERIAL_STATUS_UNAVAILABLE);
        // Wake up the open-loop task so it can drop the unique_ptr and
        // retry VCP::open() for hot-plug.
        if (s_disconnect_sem) {
            xSemaphoreGive(s_disconnect_sem);
        }
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        // Modem status lines changed. We don't expose these to the app
        // for now; could log if useful.
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        break;
    }
}

static bool cdc_data_rx_cb(const uint8_t* data, size_t len, void* /*arg*/) {
    // Update counters atomically
    s_rx_bytes.fetch_add(len);

    // Dispatch to app callback if one is registered. Use the atomic loads
    // so we capture a consistent (cb, arg) pair even if app code is
    // simultaneously swapping them.
    nanojs8_serial_rx_cb_t cb = s_rx_cb.load();
    void* arg = s_rx_cb_arg.load();
    if (cb) {
        return cb(data, len, arg);
    }

    // No callback registered — bytes are "consumed" (we don't want the
    // driver to keep them buffered indefinitely).
    return true;
}

// ---------------------------------------------------------------------------
// Open-loop task — polls VCP::open() until a CP2102 enumerates, then waits
// for disconnect, then retries. Handles hot-plug cleanly.
// ---------------------------------------------------------------------------

static void apply_line_coding_locked(uint32_t baud, uint8_t data,
                                      uint8_t parity, uint8_t stop) {
    // Caller must hold s_device_mutex and have verified s_device != nullptr.
    cdc_acm_line_coding_t lc = {
        .dwDTERate   = baud,
        .bCharFormat = stop,
        .bParityType = parity,
        .bDataBits   = data,
    };
    esp_err_t err = s_device->line_coding_set(&lc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "line_coding_set failed: %s", esp_err_to_name(err));
        return;
    }
    s_baud.store(baud);
    s_data_bits.store(data);
    s_parity.store(parity);
    s_stop_bits.store(stop);
}

static void open_loop_task(void* /*arg*/) {
    // Wait briefly for audio.cpp to have finished installing the USB host
    // library. nanojs8_audio_start() returns before its worker task has
    // finished calling usb_host_install(); if we install cdc_acm_host
    // before that happens, the install fails.
    //
    // 1.5 seconds is comfortable margin — empirically the host install
    // completes within ~50 ms of audio_start() in the L5 sustain test
    // logs, so this is 30x the observed install time.
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Install CDC-ACM host driver. Done HERE rather than in
    // nanojs8_serial_start() so we know the USB host library has
    // finished installing by now.
    esp_err_t err = cdc_acm_host_install(nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s",
                 esp_err_to_name(err));
        s_status.store(NANOJS8_SERIAL_STATUS_ERROR);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    VCP::register_driver<CP210x>();
    ESP_LOGI(TAG, "CP210x VCP driver registered");

    ESP_LOGI(TAG, "Open-loop task entered; looking for CP2102");

    while (true) {
        s_status.store(NANOJS8_SERIAL_STATUS_OPENING);

        const cdc_acm_host_device_config_t dev_cfg = {
            .connection_timeout_ms = NANOJS8_SERIAL_CONN_TIMEOUT_MS,
            .out_buffer_size       = NANOJS8_SERIAL_TX_BUFFER,
            .in_buffer_size        = NANOJS8_SERIAL_RX_BUFFER,
            .event_cb              = cdc_event_cb,
            .data_cb               = cdc_data_rx_cb,
            .user_arg              = nullptr,
        };

        // VCP::open returns nullptr if no recognized VCP device shows up
        // before the connection_timeout_ms expires.
        std::unique_ptr<CdcAcmDevice> vcp(VCP::open(&dev_cfg));
        if (vcp == nullptr) {
            // No device yet. Quiet log spam: only log once per ~30s.
            static int waited_s = 0;
            waited_s += (NANOJS8_SERIAL_CONN_TIMEOUT_MS / 1000);
            if (waited_s % 30 == 5) {
                ESP_LOGI(TAG, "No CP2102 yet (waited %ds)", waited_s);
            }
            continue;
        }

        ESP_LOGI(TAG, "CP2102 opened; setting default line coding");

        // Take the mutex to publish the new device pointer atomically with
        // the line-coding-set call.
        xSemaphoreTake(s_device_mutex, portMAX_DELAY);
        s_device = vcp.get();
        apply_line_coding_locked(s_baud.load(), s_data_bits.load(),
                                  s_parity.load(), s_stop_bits.load());

        // Apply the saved DTR/RTS state so PTT survives reconnects.
        s_device->set_control_line_state(s_dtr.load(), s_rts.load());
        xSemaphoreGive(s_device_mutex);

        s_status.store(NANOJS8_SERIAL_STATUS_READY);
        ESP_LOGI(TAG, "Serial ready: %lu baud, %u data, %s parity, %s stop",
                 (unsigned long)s_baud.load(), s_data_bits.load(),
                 (s_parity.load() == 0) ? "no" : "yes",
                 (s_stop_bits.load() == 0) ? "1" :
                 (s_stop_bits.load() == 1) ? "1.5" : "2");

        // Block here until the device disconnects. The event_cb will give
        // the semaphore. Note we do NOT hold s_device_mutex during this
        // wait — that would block all public API calls.
        xSemaphoreTake(s_disconnect_sem, portMAX_DELAY);

        // Disconnect happened. Clear the device pointer under the mutex so
        // no in-flight public API call can use the freed object.
        xSemaphoreTake(s_device_mutex, portMAX_DELAY);
        s_device = nullptr;
        s_vid.store(0);
        s_pid.store(0);
        xSemaphoreGive(s_device_mutex);

        // unique_ptr destructor runs here, which calls cdc_acm_host_close()
        ESP_LOGI(TAG, "Device closed; will retry");
    }
}

// ---------------------------------------------------------------------------
// Public API — all calls take s_device_mutex around CdcAcmDevice access
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_serial_start(void) {
    if (s_started) return ESP_OK;

    ESP_LOGI(TAG, "Starting USB serial subsystem");

    s_device_mutex   = xSemaphoreCreateMutex();
    s_disconnect_sem = xSemaphoreCreateBinary();
    if (!s_device_mutex || !s_disconnect_sem) {
        ESP_LOGE(TAG, "Semaphore create failed");
        return ESP_ERR_NO_MEM;
    }

    // NOTE: cdc_acm_host_install() and VCP::register_driver<CP210x>() are
    // deliberately deferred to open_loop_task. They require the USB host
    // library (installed by audio.cpp) to be up first, and audio_start()
    // does that asynchronously. The task waits 1.5 seconds before
    // installing, which is far longer than the observed host install time.

    BaseType_t ok = xTaskCreatePinnedToCore(
        open_loop_task, "serial_open", OPEN_TASK_STACK, nullptr,
        OPEN_TASK_PRIORITY, nullptr, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "open_loop_task create failed");
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}

extern "C" void nanojs8_serial_get_info(nanojs8_serial_info_t* info) {
    if (!info) return;
    info->status      = s_status.load();
    info->baud_rate   = s_baud.load();
    info->data_bits   = s_data_bits.load();
    info->stop_bits   = s_stop_bits.load();
    info->parity      = s_parity.load();
    info->dtr_active  = s_dtr.load();
    info->rts_active  = s_rts.load();
    info->ptt_line    = s_ptt_line.load();
    info->vid         = s_vid.load();
    info->pid         = s_pid.load();
}

extern "C" uint64_t nanojs8_serial_tx_bytes_total(void) {
    return s_tx_bytes.load();
}

extern "C" uint64_t nanojs8_serial_rx_bytes_total(void) {
    return s_rx_bytes.load();
}

extern "C" esp_err_t nanojs8_serial_set_baud(uint32_t baud) {
    return nanojs8_serial_set_line(baud, s_data_bits.load(),
                                    s_parity.load(), s_stop_bits.load());
}

extern "C" esp_err_t nanojs8_serial_set_line(uint32_t baud, uint8_t data_bits,
                                              uint8_t parity, uint8_t stop_bits) {
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (!s_device) {
        // Save the desired values so they apply on next connect.
        s_baud.store(baud);
        s_data_bits.store(data_bits);
        s_parity.store(parity);
        s_stop_bits.store(stop_bits);
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    apply_line_coding_locked(baud, data_bits, parity, stop_bits);
    xSemaphoreGive(s_device_mutex);
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_serial_write(const uint8_t* data, size_t len,
                                           uint32_t timeout_ms) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (!s_device) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    // tx_blocking takes non-const data buffer; cast our const-correct API.
    esp_err_t err = s_device->tx_blocking(const_cast<uint8_t*>(data),
                                           len, timeout_ms);
    if (err == ESP_OK) {
        s_tx_bytes.fetch_add(len);
    }
    xSemaphoreGive(s_device_mutex);
    return err;
}

extern "C" esp_err_t nanojs8_serial_set_rx_callback(nanojs8_serial_rx_cb_t cb,
                                                     void* arg) {
    // The atomic store order matters slightly: write arg first so it's
    // visible by the time the cb pointer is published. With seq_cst
    // (default), the ordering is guaranteed.
    s_rx_cb_arg.store(arg);
    s_rx_cb.store(cb);
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_serial_set_dtr(bool active) {
    s_dtr.store(active);
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (!s_device) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_device->set_control_line_state(s_dtr.load(),
                                                      s_rts.load());
    xSemaphoreGive(s_device_mutex);
    return err;
}

extern "C" esp_err_t nanojs8_serial_set_rts(bool active) {
    s_rts.store(active);
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (!s_device) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_device->set_control_line_state(s_dtr.load(),
                                                      s_rts.load());
    xSemaphoreGive(s_device_mutex);
    return err;
}

extern "C" void nanojs8_serial_ptt_line_set(nanojs8_serial_ptt_line_t line) {
    s_ptt_line.store(line);
}

extern "C" esp_err_t nanojs8_serial_ptt_set(bool transmitting) {
    // Default PTT line is RTS (per Layer 5b spec). If app code changed it
    // to DTR via nanojs8_serial_ptt_line_set(), use DTR instead.
    nanojs8_serial_ptt_line_t line = s_ptt_line.load();
    if (line == NANOJS8_SERIAL_PTT_DTR) {
        return nanojs8_serial_set_dtr(transmitting);
    }
    return nanojs8_serial_set_rts(transmitting);
}
