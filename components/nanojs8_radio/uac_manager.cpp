// NanoJS8 — UAC (USB Audio Class) manager
//
// Owns the CM108 audio device side of the radio interface. For Phase
// 3a (DigiRig profile, mono 16-bit 48 kHz), this:
//
//   1. Installs the uac_host class driver
//   2. Waits for an RX_CONNECTED event (mic-side device appears)
//   3. Opens the UAC interface, requests the profile-specified format
//   4. Spawns a dedicated streaming task that polls
//      uac_host_device_read() and feeds the ring buffer
//   5. On disconnect: stops & closes the device, drains the ring
//
// Phase 5 will add the TX-side (UAC speaker) hooking; the public
// uac_tx_write() function below is a stub that returns sample_count
// without doing anything.
//
// References:
//   - Mini-FT8 stream_uac.cpp (AG6AQ/N6HAN): proven working pattern
//     on the same Cardputer ADV / ESP-IDF v5.5.4 combination.

#include <atomic>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "usb/uac_host.h"

#include "radio_profile.h"
#include "radio_rx_buffer.h"

namespace nanojs8 {
namespace radio {

static const char* TAG = "radio_uac";

// ---------------------------------------------------------------------------
// Task / buffer constants
// ---------------------------------------------------------------------------

// UAC driver internal ringbuffer (between USB stack and our task). 16k
// matches Mini-FT8's verified-working value. Threshold = 1k bytes ≈
// 5 ms of mono 16-bit @ 48 kHz; the device callback fires when the
// driver has at least that much queued, which gives the streaming task
// reasonably balanced chunks without thrashing.
static constexpr uint32_t UAC_DRV_BUFFER_SIZE      = 16 * 1024;
static constexpr uint32_t UAC_DRV_BUFFER_THRESHOLD = 1024;

// Streaming task polling buffer — sized to ~20 ms at mono 16-bit 48 kHz
// (960 samples = 1920 bytes), rounded up to a round number. Sample-count
// in this buffer × bytes-per-sample must always fit the format we
// negotiate. 4 KB is comfortable for mono 16-bit; would be too small
// for stereo 24-bit (but we don't negotiate that for digirig_unknown).
static constexpr size_t POLL_BUFFER_BYTES = 4096;

static constexpr UBaseType_t UAC_TASK_PRIORITY     = 5;
static constexpr UBaseType_t STREAM_TASK_PRIORITY  = 4;
static constexpr uint32_t    STREAM_TASK_STACK     = 4096;
static constexpr BaseType_t  TASK_CORE             = 1;

// ---------------------------------------------------------------------------
// State (singleton)
// ---------------------------------------------------------------------------

static std::atomic<bool>           s_installed{false};
static std::atomic<bool>           s_streaming{false};
static std::atomic<bool>           s_stop_requested{false};
static uac_host_device_handle_t    s_rx_handle      = nullptr;
static TaskHandle_t                s_stream_task    = nullptr;
static QueueHandle_t               s_event_queue    = nullptr;

// Negotiated stream format (latched when uac_host_device_start succeeds).
static uint32_t s_active_sample_rate = 0;
static uint8_t  s_active_bit_depth   = 0;
static uint8_t  s_active_channels    = 0;

// Internal event queue events. The UAC driver invokes callbacks from
// its own task context; we queue them and process from our class-driver
// task to keep callback durations short.
enum class UacEvtType : uint8_t { DRIVER, DEVICE, STOP };
struct UacEvent {
    UacEvtType type;
    union {
        struct {
            uint8_t                 addr;
            uint8_t                 iface_num;
            uac_host_driver_event_t event;
        } driver;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t  event;
        } device;
    };
};

// ---------------------------------------------------------------------------
// Callbacks (invoked by UAC driver from its background task)
// ---------------------------------------------------------------------------

static void uac_driver_cb(uint8_t addr, uint8_t iface_num,
                           const uac_host_driver_event_t event, void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "UAC driver event: addr=%u iface=%u type=%d",
             addr, iface_num, (int)event);
    UacEvent ev = {};
    ev.type             = UacEvtType::DRIVER;
    ev.driver.addr      = addr;
    ev.driver.iface_num = iface_num;
    ev.driver.event     = event;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &ev, 0);
    }
}

static void uac_device_cb(uac_host_device_handle_t handle,
                           const uac_host_device_event_t event, void* arg) {
    (void)arg;
    UacEvent ev = {};
    ev.type          = UacEvtType::DEVICE;
    ev.device.handle = handle;
    ev.device.event  = event;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &ev, 0);
    }
}

// ---------------------------------------------------------------------------
// Streaming task — polls device_read into the RX ring buffer
// ---------------------------------------------------------------------------

static void stream_task_entry(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "UAC streaming task started on core %d", xPortGetCoreID());

    // Polling buffer in BSS to avoid heap fragmentation issues at task
    // start. The UAC driver requires a uint8_t* buffer.
    static uint8_t s_poll_buf[POLL_BUFFER_BYTES];

    while (!s_stop_requested.load(std::memory_order_acquire) &&
           s_rx_handle != nullptr) {
        uint32_t bytes_read = 0;
        esp_err_t err = uac_host_device_read(
            s_rx_handle, s_poll_buf, POLL_BUFFER_BYTES,
            &bytes_read, pdMS_TO_TICKS(200));

        if (err == ESP_ERR_TIMEOUT) {
            // No data ready within the timeout. Loop and try again
            // — happens at startup before the device's first packet
            // arrives, and during pauses.
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uac_host_device_read: %s", esp_err_to_name(err));
            // Continue rather than break — the device may recover.
            // If it's a real disconnect, we'll get the device event
            // shortly and the outer task will tear us down.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (bytes_read == 0) {
            continue;
        }

        // The negotiated format for Phase 3a is mono 16-bit. The driver
        // hands us little-endian PCM matching that. We can cast and
        // pass straight to the ring buffer.
        //
        // Defensive check: if for any reason we ended up negotiating a
        // different format (24-bit / stereo / etc — shouldn't happen
        // for the digirig_unknown profile but might if we add new
        // profiles), skip writing and log once. Better silence than
        // garbled audio into the ring.
        if (s_active_bit_depth != 16 || s_active_channels != 1) {
            static bool warned = false;
            if (!warned) {
                ESP_LOGE(TAG, "Active format %d-bit %d-ch not supported "
                              "by Phase 3a ring writer; discarding audio.",
                         (int)s_active_bit_depth, (int)s_active_channels);
                warned = true;
            }
            continue;
        }

        const int16_t* samples     = reinterpret_cast<const int16_t*>(s_poll_buf);
        const size_t   sample_count = bytes_read / sizeof(int16_t);
        rx_buffer_write(samples, sample_count);
    }

    ESP_LOGI(TAG, "UAC streaming task exiting");
    s_streaming.store(false, std::memory_order_release);
    s_stream_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Class-driver task — handles events from the UAC driver callback
// ---------------------------------------------------------------------------

static const RadioProfile* s_active_profile = nullptr;
static TaskHandle_t        s_class_task     = nullptr;

static void try_start_stream_for_rx(uint8_t addr, uint8_t iface_num) {
    if (s_rx_handle) {
        ESP_LOGW(TAG, "Already have an RX device; ignoring new connect");
        return;
    }
    if (!s_active_profile) {
        ESP_LOGW(TAG, "No active profile; ignoring UAC connect");
        return;
    }

    uac_host_device_config_t dev_cfg = {};
    dev_cfg.addr             = addr;
    dev_cfg.iface_num        = iface_num;
    dev_cfg.buffer_size      = UAC_DRV_BUFFER_SIZE;
    dev_cfg.buffer_threshold = UAC_DRV_BUFFER_THRESHOLD;
    dev_cfg.callback         = uac_device_cb;
    dev_cfg.callback_arg     = nullptr;

    uac_host_device_handle_t handle = nullptr;
    esp_err_t err = uac_host_device_open(&dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }

    uac_host_printf_device_param(handle);

    // Build candidate formats. For digirig_unknown, the CM108 is
    // strictly mono 16-bit 48 kHz — but some chip clones lie about
    // their alt settings. We try the profile's preferred format first;
    // if it fails, fall back through wider alternatives so the operator
    // can at least get audio rather than a hard "format not supported"
    // error.
    struct Candidate { uint8_t channels; uint8_t bits; uint32_t rate; };
    Candidate candidates[] = {
        { s_active_profile->audio.channels,
          s_active_profile->audio.bit_depth,
          s_active_profile->audio.sample_rate_hz },
        { 1, 16, 48000 },
        { 2, 16, 48000 },
        { 1, 24, 48000 },
    };
    // De-duplicate the first entry against the others if it happens to
    // match (so we don't try the same format twice).
    constexpr size_t cand_count = sizeof(candidates)/sizeof(candidates[0]);

    bool started = false;
    for (size_t i = 0; i < cand_count; ++i) {
        // skip duplicates (cheap O(n^2) but n=4)
        bool dup = false;
        for (size_t j = 0; j < i; ++j) {
            if (candidates[j].channels == candidates[i].channels &&
                candidates[j].bits     == candidates[i].bits     &&
                candidates[j].rate     == candidates[i].rate) {
                dup = true; break;
            }
        }
        if (dup) continue;

        uac_host_stream_config_t stream_cfg = {};
        stream_cfg.channels       = candidates[i].channels;
        stream_cfg.bit_resolution = candidates[i].bits;
        stream_cfg.sample_freq    = candidates[i].rate;
        stream_cfg.flags          = 0;

        ESP_LOGI(TAG, "Trying UAC format candidate %u/%u: %lu Hz, %u-bit, %u-ch",
                 (unsigned)(i + 1), (unsigned)cand_count,
                 (unsigned long)candidates[i].rate,
                 (unsigned)candidates[i].bits,
                 (unsigned)candidates[i].channels);

        err = uac_host_device_start(handle, &stream_cfg);
        if (err == ESP_OK) {
            s_active_sample_rate = candidates[i].rate;
            s_active_bit_depth   = candidates[i].bits;
            s_active_channels    = candidates[i].channels;
            ESP_LOGI(TAG, "Stream started: %lu Hz, %u-bit, %u-ch",
                     (unsigned long)s_active_sample_rate,
                     (unsigned)s_active_bit_depth,
                     (unsigned)s_active_channels);
            started = true;
            break;
        }
        ESP_LOGW(TAG, "Candidate failed: %s", esp_err_to_name(err));
    }

    if (!started) {
        ESP_LOGE(TAG, "All UAC format candidates rejected by device");
        uac_host_device_close(handle);
        return;
    }

    s_rx_handle = handle;
    s_streaming.store(true, std::memory_order_release);

    // Spawn streaming task.
    if (s_stream_task == nullptr) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            stream_task_entry, "radio_uac_rx",
            STREAM_TASK_STACK, nullptr,
            STREAM_TASK_PRIORITY, &s_stream_task, TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Stream task create failed");
            uac_host_device_stop(handle);
            uac_host_device_close(handle);
            s_rx_handle = nullptr;
            s_streaming.store(false, std::memory_order_release);
        }
    }
}

static void close_active_device() {
    if (s_rx_handle) {
        uac_host_device_stop(s_rx_handle);
        uac_host_device_close(s_rx_handle);
        s_rx_handle = nullptr;
    }
    s_streaming.store(false, std::memory_order_release);
    rx_buffer_drain();
}

static void class_task_entry(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "UAC class-driver task started on core %d", xPortGetCoreID());

    uac_host_driver_config_t drv_cfg = {};
    drv_cfg.create_background_task = true;
    drv_cfg.task_priority          = UAC_TASK_PRIORITY;
    drv_cfg.stack_size             = 4096;
    drv_cfg.core_id                = TASK_CORE;
    drv_cfg.callback               = uac_driver_cb;
    drv_cfg.callback_arg           = nullptr;

    esp_err_t err = uac_host_install(&drv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        s_installed.store(false, std::memory_order_release);
        s_class_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    s_installed.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "UAC class driver installed");

    UacEvent ev;
    while (!s_stop_requested.load(std::memory_order_acquire)) {
        if (xQueueReceive(s_event_queue, &ev, pdMS_TO_TICKS(100))) {
            switch (ev.type) {
                case UacEvtType::STOP:
                    goto done;
                case UacEvtType::DRIVER:
                    if (ev.driver.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
                        try_start_stream_for_rx(ev.driver.addr,
                                                ev.driver.iface_num);
                    } else if (ev.driver.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
                        ESP_LOGI(TAG, "TX interface present (Phase 5 will wire)");
                    }
                    break;
                case UacEvtType::DEVICE:
                    if (ev.device.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
                        ESP_LOGI(TAG, "UAC device disconnected");
                        close_active_device();
                    } else if (ev.device.event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
                        ESP_LOGW(TAG, "UAC transfer error");
                    }
                    break;
            }
        }
    }
done:
    close_active_device();

    uac_host_uninstall();
    s_installed.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "UAC class driver uninstalled");
    s_class_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API used by radio_service.cpp
// ---------------------------------------------------------------------------

esp_err_t uac_manager_start(const RadioProfile* profile) {
    if (!profile) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_installed.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "uac_manager_start called but already running");
        return ESP_OK;
    }

    s_active_profile = profile;
    s_stop_requested.store(false, std::memory_order_release);

    if (!s_event_queue) {
        s_event_queue = xQueueCreate(8, sizeof(UacEvent));
        if (!s_event_queue) {
            ESP_LOGE(TAG, "Event queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!rx_buffer_init()) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        class_task_entry, "radio_uac_drv",
        4096, nullptr,
        UAC_TASK_PRIORITY, &s_class_task, TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "UAC class-driver task create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void uac_manager_stop(void) {
    if (!s_class_task) return;
    UacEvent ev = {};
    ev.type = UacEvtType::STOP;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &ev, 0);
    }
    s_stop_requested.store(true, std::memory_order_release);
    // Class task will exit; stream task exits via shared flag.
    // We don't synchronously wait — radio_service::stop() does an
    // overall delay-and-poll.
}

bool uac_manager_is_streaming(void) {
    return s_streaming.load(std::memory_order_acquire);
}

// Phase 5 will replace this with a real implementation that pushes
// samples into the UAC TX stream. Phase 3a accepts but discards.
size_t uac_manager_tx_write(const int16_t* src, size_t sample_count) {
    (void)src;
    return sample_count;  // pretend to consume so callers don't spin
}

} // namespace radio
} // namespace nanojs8
