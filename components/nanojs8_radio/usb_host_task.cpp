// NanoJS8 — USB host library task
//
// Owns the USB host library lifecycle:
//
//   1. usb_host_install() — sets up the OTG controller as host. Because
//      Phase 3a moves the console to UART0/Grove (per design decision),
//      the USB-Serial-JTAG controller is NOT consuming the internal PHY.
//      The OTG controller can therefore claim the PHY at install time
//      without any runtime register manipulation. This is the same
//      pattern Mini-FT8 uses.
//
//   2. Event loop — drains usb_host_lib_handle_events() forever, with
//      the standard NO_CLIENTS / ALL_FREE event handling so client
//      drivers (UAC, CDC) can be uninstalled cleanly when we tear down.
//
//   3. Tear-down — uninstalls the host stack and exits the task.
//
// This file deliberately knows NOTHING about radio profiles, UAC, or
// CDC. It's just the bus-level plumbing. The profile-specific event
// handling lives in uac_manager.cpp and cp2102_ptt.cpp.

#include <atomic>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace nanojs8 {
namespace radio {

static const char* TAG = "radio_usb";

// Task parameters. Matches Mini-FT8's working configuration.
static constexpr UBaseType_t USB_HOST_TASK_PRIORITY = 5;
static constexpr uint32_t    USB_HOST_TASK_STACK    = 4096;
static constexpr BaseType_t  USB_HOST_TASK_CORE     = 1;

// State.
static TaskHandle_t          s_task_handle    = nullptr;
static std::atomic<bool>     s_stop_requested{false};
static std::atomic<bool>     s_host_installed{false};

// Task entry. Runs forever until s_stop_requested is set true and the
// host library reports ALL_FREE.
static void usb_host_task_entry(void* arg) {
    TaskHandle_t notify_target = (TaskHandle_t)arg;

    // Install USB host library. skip_phy_setup=false → driver configures
    // the OTG PHY for us. Works on Cardputer ADV because our console is
    // on UART0/Grove (GPIO 1/2), not on USB-Serial-JTAG which would
    // otherwise be holding the PHY.
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup     = false;
    host_config.intr_flags         = ESP_INTR_FLAG_LEVEL1;

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        if (notify_target) {
            // Use a notify value of 0 to mean "install failed".
            xTaskNotify(notify_target, 0, eSetValueWithOverwrite);
        }
        s_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    s_host_installed.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "USB host library installed");

    // Notify the start() caller that host is up so it can proceed to
    // install class drivers (UAC, CDC). Notify value 1 means "ok".
    if (notify_target) {
        xTaskNotify(notify_target, 1, eSetValueWithOverwrite);
    }

    // Event loop. usb_host_lib_handle_events() blocks up to the timeout
    // waiting for any host-stack event; the event_flags out parameter
    // tells us what happened.
    //
    // The two events we care about:
    //   NO_CLIENTS: all client drivers have detached. Time to free any
    //               remaining device handles.
    //   ALL_FREE:   all device handles released. Safe to uninstall.
    //
    // We don't uninstall on NO_CLIENTS — the user may unplug + replug
    // and we want to keep the host running. We only uninstall when the
    // outer service tells us to stop.
    while (!s_stop_requested.load(std::memory_order_acquire)) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (err == ESP_OK) {
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGI(TAG, "No USB clients");
                usb_host_device_free_all();
            }
            // ALL_FREE in the normal-run loop just means there are no
            // devices currently enumerated; no action needed.
        } else if (err != ESP_ERR_TIMEOUT) {
            // Unexpected error; log but keep running.
            ESP_LOGW(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(err));
        }
    }

    // Shutdown sequence — drain pending events until ALL_FREE before
    // uninstalling. This mirrors Mini-FT8's careful tear-down which
    // avoided a hang when re-installing TinyUSB device mode afterwards.
    ESP_LOGI(TAG, "USB host stop requested; draining events");
    for (int i = 0; i < 50; ++i) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(20), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }

    err = usb_host_uninstall();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_host_uninstall: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "USB host uninstalled");
    }

    s_host_installed.store(false, std::memory_order_release);
    s_task_handle = nullptr;
    vTaskDelete(nullptr);
}

// Public — start the USB host task. Blocks until install completes
// or fails. Returns ESP_OK on success.
esp_err_t usb_host_start(void) {
    if (s_host_installed.load(std::memory_order_acquire)) {
        return ESP_OK;  // idempotent
    }
    if (s_task_handle) {
        ESP_LOGW(TAG, "usb_host_start: task already exists");
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested.store(false, std::memory_order_release);
    const TaskHandle_t notify_target = xTaskGetCurrentTaskHandle();

    BaseType_t ok = xTaskCreatePinnedToCore(
        usb_host_task_entry,
        "radio_usb_host",
        USB_HOST_TASK_STACK,
        (void*)notify_target,
        USB_HOST_TASK_PRIORITY,
        &s_task_handle,
        USB_HOST_TASK_CORE
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        s_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Wait up to 5 s for the task to confirm install. The blocking
    // wait is on a task notification posted by the task entry.
    uint32_t notify_value = 0;
    const BaseType_t taken = xTaskNotifyWait(
        0, 0xFFFFFFFFUL, &notify_value, pdMS_TO_TICKS(5000));
    if (taken != pdTRUE) {
        ESP_LOGE(TAG, "USB host task did not signal start within 5s");
        // Best-effort: signal stop. Task may still be alive but the
        // caller should treat this as a failure.
        s_stop_requested.store(true, std::memory_order_release);
        return ESP_ERR_TIMEOUT;
    }
    if (notify_value == 0) {
        ESP_LOGE(TAG, "USB host task reported install failure");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Public — request the USB host task to shut down. Does NOT wait for
// completion — radio_service::stop() does any synchronization it needs.
void usb_host_stop(void) {
    s_stop_requested.store(true, std::memory_order_release);
}

// Public — read-only health probe used by snapshot().
bool usb_host_is_installed(void) {
    return s_host_installed.load(std::memory_order_acquire);
}

} // namespace radio
} // namespace nanojs8
