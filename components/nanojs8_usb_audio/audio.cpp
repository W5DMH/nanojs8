/*
 * audio.cpp — NanoJS8 v0.7 USB audio implementation
 * ===================================================
 * See audio.h for the public API and threading model.
 *
 * Architecture (heavily adapted from espressif/esp-iot-solution's
 * usb_audio_player example, simplified to our needs):
 *
 *   ┌───────────────────────────────────────────────────────────┐
 *   │  USB Host Library Task   (host lib events, ref. counts)   │
 *   ├───────────────────────────────────────────────────────────┤
 *   │  UAC Driver Task         (class events, per uac_host)     │
 *   ├───────────────────────────────────────────────────────────┤
 *   │  Audio Dispatcher Task   (our code — opens devices,       │
 *   │                           negotiates rates, starts streams)│
 *   └───────────────────────────────────────────────────────────┘
 *
 * Discovery algorithm (per direction):
 *   1. uac_host_device_open() with addr + iface_num from the event
 *   2. uac_host_get_device_info() — VID/PID/strings for logging
 *   3. uac_host_get_device_alt_param() for each alternate setting
 *      (iface_alt = 1..iface_alt_num)
 *   4. Among all alt settings, pick the one offering our preferred rate
 *      (48000 → 44100 → 32000 → 16000 → 8000) at 16-bit
 *   5. Build a uac_host_stream_config_t and call uac_host_device_start()
 *
 * Why this is non-trivial:
 *   - Each alternate setting has its OWN supported rate list. We have to
 *     scan all of them to find the best match.
 *   - Some alt settings may use stereo, others mono. We accept either,
 *     preferring mono (matches our channel default and saves bandwidth).
 *   - sample_freq_type=0 means "continuous range" (rare for class-compliant
 *     devices; if encountered, pick any rate in the [lower, upper] range).
 *
 * License: GPL-3.0
 */

#include "audio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "usb/usb_host.h"
#include "usb/uac_host.h"

#include <atomic>
#include <cstring>

static const char* TAG = "audio";

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

namespace {

// Task params. Matches the example's recommended priorities.
constexpr int USB_HOST_TASK_PRIORITY = 5;
constexpr int UAC_TASK_PRIORITY      = 5;
constexpr int DISP_TASK_PRIORITY     = 4;
constexpr int USB_HOST_TASK_STACK    = 4096;
constexpr int UAC_TASK_STACK         = 4096;
constexpr int DISP_TASK_STACK        = 4096;

// Per-device ring buffer sizing. The audio driver buffers up to
// BUFFER_SIZE bytes of audio internally; UAC_HOST_DEVICE_EVENT_RX_DONE
// fires when the buffer crosses BUFFER_THRESHOLD.
//
// At 48 kHz mono 16-bit: 96000 B/s. 16 KB = ~170 ms of audio buffer,
// 4 KB threshold = ~42 ms per event. Plenty of headroom for our loopback.
constexpr uint32_t AUDIO_BUFFER_SIZE      = 16000;
constexpr uint32_t AUDIO_BUFFER_THRESHOLD = 4000;

// Preferred sample rates in priority order. Mirror the public macro
// but keep as a constexpr array for the discovery logic.
constexpr uint32_t PREFERRED_RATES[] = { 48000, 44100, 32000, 16000, 8000 };
constexpr size_t PREFERRED_RATES_COUNT =
    sizeof(PREFERRED_RATES) / sizeof(PREFERRED_RATES[0]);

// Event group enum for our internal dispatcher queue.
enum event_group_t {
    EVT_DRIVER = 0,   // From UAC driver callback (TX_CONNECTED / RX_CONNECTED)
    EVT_DEVICE,       // From per-device callback (RX_DONE / TX_DONE / DISCONNECTED / ERROR)
};

// Queue item used by both callbacks. Tagged union keyed by event_group.
struct event_msg_t {
    event_group_t group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
        } drv;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
        } dev;
    };
};

// Stream state shared between the dispatcher task and the public API.
// Marked atomic where read by other tasks; the dispatcher is the only
// writer.
struct stream_state_t {
    std::atomic<uac_host_device_handle_t> handle{nullptr};
    std::atomic<nanojs8_audio_status_t>   status{NANOJS8_AUDIO_STATUS_UNAVAILABLE};
    std::atomic<uint32_t> sample_rate{0};
    std::atomic<uint8_t>  channels{0};
    std::atomic<uint8_t>  bit_resolution{0};
    std::atomic<uint16_t> vid{0};
    std::atomic<uint16_t> pid{0};
    std::atomic<uint64_t> samples_total{0};
};

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
QueueHandle_t s_event_queue = nullptr;
TaskHandle_t  s_usb_task    = nullptr;
TaskHandle_t  s_uac_task    = nullptr;
TaskHandle_t  s_disp_task   = nullptr;
bool          s_started     = false;

stream_state_t s_rx;   // microphone
stream_state_t s_tx;   // speaker

std::atomic<uint16_t> s_rx_peak{0};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Callbacks (invoked from UAC driver task or USB host task — must not block)
// ---------------------------------------------------------------------------

static void uac_driver_cb(uint8_t addr, uint8_t iface_num,
                          const uac_host_driver_event_t event, void* /*arg*/) {
    event_msg_t msg = {};
    msg.group = EVT_DRIVER;
    msg.drv.addr = addr;
    msg.drv.iface_num = iface_num;
    msg.drv.event = event;
    // 0 timeout — must never block this callback
    xQueueSend(s_event_queue, &msg, 0);
}

static void uac_device_cb(uac_host_device_handle_t handle,
                          const uac_host_device_event_t event, void* /*arg*/) {
    event_msg_t msg = {};
    msg.group = EVT_DEVICE;
    msg.dev.handle = handle;
    msg.dev.event = event;
    xQueueSend(s_event_queue, &msg, 0);
}

// ---------------------------------------------------------------------------
// USB Host library task (boilerplate from the Espressif example)
// ---------------------------------------------------------------------------

static void usb_host_task(void* arg) {
    // Use designated initializers only for fields that exist
    // unconditionally. Any optional fields (e.g. enum_filter_cb,
    // which only exists when CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
    // is enabled) are left zero-initialized.
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "USB Host installed");

    // Notify the dispatcher task that the host is up so it can install UAC
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            // No clients registered — would happen if UAC driver uninstalled
            ESP_LOGW(TAG, "USB host: no clients, freeing devices");
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            // All devices freed — safe to uninstall, but we never do that
            ESP_LOGI(TAG, "USB host: all devices freed");
        }
    }
}

// ---------------------------------------------------------------------------
// Rate discovery — scan alternate settings and pick the best match
// ---------------------------------------------------------------------------

// Returns true if `param` advertises `rate`. Handles both the discrete
// case (sample_freq_type > 0, listed in sample_freq[]) and the continuous
// case (sample_freq_type == 0, anything in [lower, upper]).
static bool alt_supports_rate(const uac_host_dev_alt_param_t& param,
                              uint32_t rate) {
    if (param.sample_freq_type == 0) {
        // Continuous range. Rare in real devices but handle it.
        return rate >= param.sample_freq_lower &&
               rate <= param.sample_freq_upper;
    }
    // Discrete list. The driver stores up to UAC_FREQ_NUM_MAX entries;
    // sample_freq_type tells us how many are actually valid.
    uint8_t n = param.sample_freq_type;
    for (uint8_t i = 0; i < n; ++i) {
        if (param.sample_freq[i] == rate) return true;
    }
    return false;
}

// Scan all alternate settings on the device and find the best match for
// our preference list. Writes the chosen alt_index, rate, channels, bits
// to the out params. Returns true if any usable alt was found.
//
// Selection rules, in priority order:
//   1. Prefer rates earlier in PREFERRED_RATES (48k beats 44.1k)
//   2. Prefer 16-bit resolution (modem expects 16-bit PCM)
//   3. Prefer mono over stereo (halves bandwidth and matches our channel default)
static bool discover_best_alt(uac_host_device_handle_t handle,
                              uint8_t iface_alt_num,
                              uint8_t  *out_alt_index,
                              uint32_t *out_rate,
                              uint8_t  *out_channels,
                              uint8_t  *out_bit_resolution) {
    int best_rate_priority = -1;     // index into PREFERRED_RATES; lower is better
    int best_channels      = 99;     // lower (mono) is better
    uint8_t  chosen_alt = 0;
    uint32_t chosen_rate = 0;
    uint8_t  chosen_channels = 0;
    uint8_t  chosen_bits = 0;
    bool found = false;

    for (uint8_t alt = 1; alt <= iface_alt_num; ++alt) {
        uac_host_dev_alt_param_t param = {};
        esp_err_t err = uac_host_get_device_alt_param(handle, alt, &param);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "  alt %u: get_alt_param failed: %s",
                     alt, esp_err_to_name(err));
            continue;
        }

        // Log every alt setting for diagnostics
        ESP_LOGI(TAG, "  alt %u: format=%u ch=%u bits=%u subframe=%u "
                      "freq_type=%u",
                 alt, param.format, param.channels, param.bit_resolution,
                 param.subframe_size, param.sample_freq_type);
        if (param.sample_freq_type == 0) {
            ESP_LOGI(TAG, "    continuous range: %lu .. %lu Hz",
                     (unsigned long)param.sample_freq_lower,
                     (unsigned long)param.sample_freq_upper);
        } else {
            for (uint8_t i = 0; i < param.sample_freq_type; ++i) {
                ESP_LOGI(TAG, "    discrete: %lu Hz",
                         (unsigned long)param.sample_freq[i]);
            }
        }

        // Only consider 16-bit PCM (format == 1)
        if (param.format != 1 || param.bit_resolution != 16) continue;

        // Try each preferred rate
        for (size_t i = 0; i < PREFERRED_RATES_COUNT; ++i) {
            if (!alt_supports_rate(param, PREFERRED_RATES[i])) continue;

            // Better priority? (lower i = higher preference)
            bool better_rate = (best_rate_priority < 0) ||
                                (int)i < best_rate_priority;
            // Same rate priority but fewer channels = better
            bool same_rate_better_ch =
                ((int)i == best_rate_priority) &&
                (param.channels < best_channels);

            if (better_rate || same_rate_better_ch) {
                best_rate_priority = i;
                best_channels      = param.channels;
                chosen_alt         = alt;
                chosen_rate        = PREFERRED_RATES[i];
                chosen_channels    = param.channels;
                chosen_bits        = param.bit_resolution;
                found = true;
            }
            // Once a rate at this alt is found, no need to try lower-priority
            // rates within the same alt — we've already locked in the best
            // rate this alt offers.
            break;
        }
    }

    if (found) {
        *out_alt_index       = chosen_alt;
        *out_rate            = chosen_rate;
        *out_channels        = chosen_channels;
        *out_bit_resolution  = chosen_bits;
    }
    return found;
}

// ---------------------------------------------------------------------------
// Open + start a stream when the UAC driver reports CONNECTED
// ---------------------------------------------------------------------------

static void handle_connected(uint8_t addr, uint8_t iface_num,
                              bool is_rx) {
    stream_state_t& state = is_rx ? s_rx : s_tx;
    const char* dir = is_rx ? "RX (mic)" : "TX (speaker)";

    ESP_LOGI(TAG, "%s connected: addr=%u iface=%u", dir, addr, iface_num);

    // Open the logical device
    uac_host_device_config_t dev_cfg = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = AUDIO_BUFFER_SIZE,
        .buffer_threshold = AUDIO_BUFFER_THRESHOLD,
        .callback = uac_device_cb,
        .callback_arg = nullptr,
    };

    uac_host_device_handle_t handle = nullptr;
    esp_err_t err = uac_host_device_open(&dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s open failed: %s", dir, esp_err_to_name(err));
        state.status.store(NANOJS8_AUDIO_STATUS_ERROR);
        return;
    }

    // Read device info (VID/PID/strings) for logging
    uac_host_dev_info_t info = {};
    if (uac_host_get_device_info(handle, &info) == ESP_OK) {
        ESP_LOGI(TAG, "%s device: VID=0x%04X PID=0x%04X "
                      "alt_settings=%u",
                 dir, info.VID, info.PID, info.iface_alt_num);
        state.vid.store(info.VID);
        state.pid.store(info.PID);
    }

    // Get the alt_setting count via info struct (already populated)
    uint8_t alt_count = info.iface_alt_num;
    if (alt_count == 0) {
        ESP_LOGE(TAG, "%s no alt settings reported", dir);
        uac_host_device_close(handle);
        state.status.store(NANOJS8_AUDIO_STATUS_ERROR);
        return;
    }

    // Discover the best stream config across all alt settings
    uint8_t  best_alt = 0;
    uint32_t rate = 0;
    uint8_t  channels = 0;
    uint8_t  bits = 0;
    if (!discover_best_alt(handle, alt_count, &best_alt, &rate,
                            &channels, &bits)) {
        ESP_LOGE(TAG, "%s no compatible alt setting (need 16-bit PCM at "
                      "one of our preferred rates)", dir);
        uac_host_device_close(handle);
        state.status.store(NANOJS8_AUDIO_STATUS_ERROR);
        return;
    }

    ESP_LOGI(TAG, "%s chosen: alt=%u rate=%lu Hz ch=%u bits=%u",
             dir, best_alt, (unsigned long)rate, channels, bits);

    // Start the stream
    uac_host_stream_config_t stream_cfg = {
        .channels = channels,
        .bit_resolution = bits,
        .sample_freq = rate,
        .flags = 0,   // start immediately (no FLAG_STREAM_SUSPEND_AFTER_START)
    };

    err = uac_host_device_start(handle, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s start failed: %s", dir, esp_err_to_name(err));
        uac_host_device_close(handle);
        state.status.store(NANOJS8_AUDIO_STATUS_ERROR);
        return;
    }

    // Commit state
    state.handle.store(handle);
    state.sample_rate.store(rate);
    state.channels.store(channels);
    state.bit_resolution.store(bits);
    state.status.store(NANOJS8_AUDIO_STATUS_READY);

    ESP_LOGI(TAG, "%s streaming at %lu Hz / %u ch / %u-bit",
             dir, (unsigned long)rate, channels, bits);
}

static void handle_disconnected(uac_host_device_handle_t handle) {
    // Determine which direction this handle belongs to
    stream_state_t* state = nullptr;
    const char* dir = "unknown";
    if (s_rx.handle.load() == handle) {
        state = &s_rx;
        dir = "RX (mic)";
    } else if (s_tx.handle.load() == handle) {
        state = &s_tx;
        dir = "TX (speaker)";
    }
    if (!state) {
        ESP_LOGW(TAG, "Disconnect for unknown handle %p", handle);
        // Try to close anyway to free driver resources
        uac_host_device_close(handle);
        return;
    }

    ESP_LOGI(TAG, "%s disconnected", dir);
    state->status.store(NANOJS8_AUDIO_STATUS_UNAVAILABLE);
    state->handle.store(nullptr);
    state->sample_rate.store(0);
    state->channels.store(0);
    state->bit_resolution.store(0);
    state->vid.store(0);
    state->pid.store(0);
    uac_host_device_close(handle);
}

// ---------------------------------------------------------------------------
// Dispatcher task — drains the event queue
// ---------------------------------------------------------------------------

static void dispatcher_task(void* /*arg*/) {
    // Wait for usb_host_task to signal "USB host installed"
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Install the UAC driver. From this point, the UAC driver task
    // pumps events and our uac_driver_cb starts firing on enumeration.
    uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = UAC_TASK_STACK,
        .core_id = 0,
        .callback = uac_driver_cb,
        .callback_arg = nullptr,
    };

    esp_err_t err = uac_host_install(&uac_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "UAC class driver installed");

    // Drain the queue forever
    event_msg_t msg = {};
    while (true) {
        if (xQueueReceive(s_event_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.group == EVT_DRIVER) {
            // A device showed up. Decide direction.
            bool is_rx = (msg.drv.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED);
            handle_connected(msg.drv.addr, msg.drv.iface_num, is_rx);
        } else if (msg.group == EVT_DEVICE) {
            switch (msg.dev.event) {
            case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                handle_disconnected(msg.dev.handle);
                break;
            case UAC_HOST_DEVICE_EVENT_RX_DONE:
                // RX buffer crossed threshold. Consumers pull via read().
                // We don't process here; just update the counters when
                // the consumer reads.
                break;
            case UAC_HOST_DEVICE_EVENT_TX_DONE:
                // TX buffer drained below threshold. Consumers push more
                // via write().
                break;
            case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                ESP_LOGW(TAG, "Transfer error on handle %p", msg.dev.handle);
                break;
            default:
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_audio_start(void) {
    if (s_started) return ESP_OK;

    ESP_LOGI(TAG, "Starting USB audio subsystem...");

    // Event queue. 16 messages is plenty — events are infrequent (connect /
    // disconnect / done) compared to actual audio data flow.
    s_event_queue = xQueueCreate(16, sizeof(event_msg_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    // Spawn the dispatcher task FIRST so it can receive the notification
    // from the USB host task when install completes.
    BaseType_t ok = xTaskCreatePinnedToCore(
        dispatcher_task, "audio_disp", DISP_TASK_STACK, nullptr,
        DISP_TASK_PRIORITY, &s_disp_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "dispatcher_task create failed");
        vQueueDelete(s_event_queue);
        s_event_queue = nullptr;
        return ESP_FAIL;
    }

    // Spawn the USB host library task. Passes the dispatcher handle so
    // the host task can notify it once install completes.
    ok = xTaskCreatePinnedToCore(
        usb_host_task, "audio_usbh", USB_HOST_TASK_STACK,
        (void*)s_disp_task, USB_HOST_TASK_PRIORITY, &s_usb_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "usb_host_task create failed");
        vTaskDelete(s_disp_task);
        s_disp_task = nullptr;
        vQueueDelete(s_event_queue);
        s_event_queue = nullptr;
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "USB audio subsystem started");
    return ESP_OK;
}

static void fill_info(const stream_state_t& s, nanojs8_audio_stream_info_t* info) {
    info->status         = s.status.load();
    info->sample_rate    = s.sample_rate.load();
    info->channels       = s.channels.load();
    info->bit_resolution = s.bit_resolution.load();
    info->vid            = s.vid.load();
    info->pid            = s.pid.load();
}

extern "C" void nanojs8_audio_rx_info(nanojs8_audio_stream_info_t* info) {
    if (info) fill_info(s_rx, info);
}
extern "C" void nanojs8_audio_tx_info(nanojs8_audio_stream_info_t* info) {
    if (info) fill_info(s_tx, info);
}

extern "C" esp_err_t nanojs8_audio_read(uint8_t* data, uint32_t size,
                                         uint32_t* bytes_read,
                                         uint32_t timeout_ms) {
    if (!data || !bytes_read) return ESP_ERR_INVALID_ARG;
    *bytes_read = 0;

    uac_host_device_handle_t h = s_rx.handle.load();
    if (!h || s_rx.status.load() != NANOJS8_AUDIO_STATUS_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t got = 0;
    esp_err_t err = uac_host_device_read(h, data, size, &got,
                                          pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK && got > 0) {
        // Update peak (16-bit signed PCM, mono or interleaved stereo)
        uint16_t peak = 0;
        const int16_t* samples = reinterpret_cast<const int16_t*>(data);
        size_t nsamples = got / sizeof(int16_t);
        for (size_t i = 0; i < nsamples; ++i) {
            int16_t s = samples[i];
            uint16_t mag = (s < 0) ? (uint16_t)(-(int32_t)s) : (uint16_t)s;
            if (mag > peak) peak = mag;
        }
        s_rx_peak.store(peak);
        s_rx.samples_total.fetch_add(nsamples);
    }
    *bytes_read = got;
    return err;
}

extern "C" esp_err_t nanojs8_audio_write(const uint8_t* data, uint32_t size,
                                          uint32_t timeout_ms) {
    if (!data) return ESP_ERR_INVALID_ARG;

    uac_host_device_handle_t h = s_tx.handle.load();
    if (!h || s_tx.status.load() != NANOJS8_AUDIO_STATUS_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    // The driver's write API takes non-const data. We don't modify our
    // input but we have to cast away const to match the signature.
    esp_err_t err = uac_host_device_write(h, const_cast<uint8_t*>(data),
                                           size, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        s_tx.samples_total.fetch_add(size / sizeof(int16_t));
    }
    return err;
}

extern "C" uint64_t nanojs8_audio_rx_samples_total(void) {
    return s_rx.samples_total.load();
}

extern "C" uint64_t nanojs8_audio_tx_samples_total(void) {
    return s_tx.samples_total.load();
}

extern "C" uint16_t nanojs8_audio_rx_peak(void) {
    return s_rx_peak.load();
}
