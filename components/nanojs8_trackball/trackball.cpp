/*
 * trackball.cpp — NanoJS8 v0.7 trackball implementation
 * =======================================================
 * See trackball.h for the public API.
 *
 * Implementation notes:
 *
 * 1. We use the ESP-IDF GPIO ISR service (gpio_install_isr_service)
 *    rather than a separate ISR per pin. The service maintains one
 *    global ISR dispatcher and per-pin handler callbacks — saves
 *    flash space and is the recommended pattern.
 *
 * 2. The ISR handlers run in interrupt context. They:
 *      a. Read the current pin level
 *      b. Compare to the last-known level (per pin)
 *      c. If changed AND debounce interval has elapsed, post an event
 *      d. Update last-known level and last-tick timestamp
 *    No printing, no logging, no malloc — all forbidden from ISR.
 *
 * 3. The debounce interval is measured in microseconds via
 *    esp_timer_get_time(). Cheaper than xTaskGetTickCountFromISR
 *    when we just need a monotonic clock.
 *
 * 4. The center-click is a momentary press (active-low). We post a
 *    CLICK event on the FALLING edge only — we don't fire on release.
 *    Same debounce interval as direction pins.
 *
 * License: GPL-3.0
 */

#include "trackball.h"
#include "platform_tdeck.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>

static const char* TAG = "trackball";

namespace {

// File-static state
QueueHandle_t s_event_queue = nullptr;
bool          s_started     = false;

// Counters per event type (UP/DOWN/LEFT/RIGHT/CLICK). Indexed by
// event code minus NANOJS8_TRACKBALL_UP. Atomic so we can read from
// any task without locking.
std::atomic<uint32_t> s_count_up{0};
std::atomic<uint32_t> s_count_down{0};
std::atomic<uint32_t> s_count_left{0};
std::atomic<uint32_t> s_count_right{0};
std::atomic<uint32_t> s_count_click{0};

// Per-pin debounce state. Indexed by direction (UP/DOWN/LEFT/RIGHT/CLICK).
// last_level: the pin level seen at the most recent accepted edge.
// last_us:    timestamp (esp_timer_get_time) of that accepted edge.
struct pin_state_t {
    volatile int     last_level;   // 0 or 1, accessed from ISR
    volatile int64_t last_us;       // esp_timer_get_time at last event
};

pin_state_t s_pin_up    = {1, 0};   // pull-up → idle high
pin_state_t s_pin_down  = {1, 0};
pin_state_t s_pin_left  = {1, 0};
pin_state_t s_pin_right = {1, 0};
pin_state_t s_pin_click = {1, 0};

// ISR helper: common edge handler. Reads the pin, checks debounce,
// updates state, posts a virtual-key event to the queue.
// Direction handlers use ANY edge as a tick. Click handler invokes
// this only on the falling edge.
IRAM_ATTR static void post_event_from_isr(uint8_t event_code,
                                           pin_state_t* state,
                                           int pin,
                                           std::atomic<uint32_t>* counter) {
    int level = gpio_get_level(static_cast<gpio_num_t>(pin));
    int64_t now_us = esp_timer_get_time();

    // Debounce: ignore events on the same pin closer than the threshold.
    if (now_us - state->last_us <
        (int64_t)NANOJS8_TRACKBALL_DEBOUNCE_MS * 1000) {
        // Update last_level so we don't lose track of the steady state,
        // but don't fire an event.
        state->last_level = level;
        return;
    }

    // The direction pins toggle on each tick; only fire when level
    // actually changed since the last accepted event. Without this
    // check, a single physical tick that bounces back-and-forth could
    // produce a stream of events at the debounce-interval rate.
    if (level == state->last_level) {
        return;
    }

    state->last_level = level;
    state->last_us    = now_us;

    counter->fetch_add(1);

    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_event_queue, &event_code, &hpw);
    if (hpw == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

IRAM_ATTR static void isr_up(void* /*arg*/) {
    post_event_from_isr(NANOJS8_TRACKBALL_UP, &s_pin_up,
                         nanojs8::platform::tdeck::PIN_TRACKBALL_UP,
                         &s_count_up);
}
IRAM_ATTR static void isr_down(void* /*arg*/) {
    post_event_from_isr(NANOJS8_TRACKBALL_DOWN, &s_pin_down,
                         nanojs8::platform::tdeck::PIN_TRACKBALL_DOWN,
                         &s_count_down);
}
IRAM_ATTR static void isr_left(void* /*arg*/) {
    post_event_from_isr(NANOJS8_TRACKBALL_LEFT, &s_pin_left,
                         nanojs8::platform::tdeck::PIN_TRACKBALL_LEFT,
                         &s_count_left);
}
IRAM_ATTR static void isr_right(void* /*arg*/) {
    post_event_from_isr(NANOJS8_TRACKBALL_RIGHT, &s_pin_right,
                         nanojs8::platform::tdeck::PIN_TRACKBALL_RIGHT,
                         &s_count_right);
}

// Click handler: post only on the FALLING edge (press, not release).
// Implemented as negedge interrupt rather than the any-edge logic
// the direction pins use.
IRAM_ATTR static void isr_click(void* /*arg*/) {
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_pin_click.last_us <
        (int64_t)NANOJS8_TRACKBALL_DEBOUNCE_MS * 1000) {
        return;
    }
    s_pin_click.last_us = now_us;
    s_count_click.fetch_add(1);

    uint8_t ev = NANOJS8_TRACKBALL_CLICK;
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_event_queue, &ev, &hpw);
    if (hpw == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Common GPIO config for a direction pin: input, internal pull-up,
// any-edge interrupt. Returns ESP_OK or the underlying GPIO error.
esp_err_t configure_direction_pin(int pin, gpio_isr_t handler) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_ANYEDGE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_isr_handler_add(static_cast<gpio_num_t>(pin),
                                 handler, nullptr);
}

esp_err_t configure_click_pin(int pin) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_NEGEDGE;   // press only, not release
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_isr_handler_add(static_cast<gpio_num_t>(pin),
                                 isr_click, nullptr);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_trackball_start(void) {
    using namespace nanojs8::platform::tdeck;

    if (s_started) return ESP_OK;

    ESP_LOGI(TAG, "Starting trackball subsystem");
    ESP_LOGI(TAG, "  Pins: UP=%d DOWN=%d LEFT=%d RIGHT=%d CLICK=%d",
             PIN_TRACKBALL_UP, PIN_TRACKBALL_DOWN,
             PIN_TRACKBALL_LEFT, PIN_TRACKBALL_RIGHT,
             PIN_TRACKBALL_CLICK);

    // Create the event queue first — ISRs will start firing as soon
    // as we install handlers.
    s_event_queue = xQueueCreate(NANOJS8_TRACKBALL_QUEUE_DEPTH,
                                  sizeof(uint8_t));
    if (s_event_queue == nullptr) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    // Install the ISR service. The flag value matches the audio
    // subsystem's USB host install (LEVEL1). gpio_install_isr_service
    // is idempotent — if already installed by another component, it
    // returns ESP_ERR_INVALID_STATE which we treat as success.
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s",
                 esp_err_to_name(err));
        vQueueDelete(s_event_queue);
        s_event_queue = nullptr;
        return err;
    }

    // Seed each pin's last_level with the current state. This way the
    // FIRST tick after boot doesn't fire just because the idle state
    // happened to be different from our default of 1.
    s_pin_up.last_level    = gpio_get_level(
        static_cast<gpio_num_t>(PIN_TRACKBALL_UP));
    s_pin_down.last_level  = gpio_get_level(
        static_cast<gpio_num_t>(PIN_TRACKBALL_DOWN));
    s_pin_left.last_level  = gpio_get_level(
        static_cast<gpio_num_t>(PIN_TRACKBALL_LEFT));
    s_pin_right.last_level = gpio_get_level(
        static_cast<gpio_num_t>(PIN_TRACKBALL_RIGHT));
    s_pin_click.last_level = gpio_get_level(
        static_cast<gpio_num_t>(PIN_TRACKBALL_CLICK));

    // Configure each pin individually. If any one fails, we abort and
    // leave the trackball disabled — the rest of the firmware still runs.
    err = configure_direction_pin(PIN_TRACKBALL_UP, isr_up);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config UP pin %d failed: %s",
                 PIN_TRACKBALL_UP, esp_err_to_name(err));
        return err;
    }
    err = configure_direction_pin(PIN_TRACKBALL_DOWN, isr_down);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config DOWN pin %d failed: %s",
                 PIN_TRACKBALL_DOWN, esp_err_to_name(err));
        return err;
    }
    err = configure_direction_pin(PIN_TRACKBALL_LEFT, isr_left);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config LEFT pin %d failed: %s",
                 PIN_TRACKBALL_LEFT, esp_err_to_name(err));
        return err;
    }
    err = configure_direction_pin(PIN_TRACKBALL_RIGHT, isr_right);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config RIGHT pin %d failed: %s",
                 PIN_TRACKBALL_RIGHT, esp_err_to_name(err));
        return err;
    }
    err = configure_click_pin(PIN_TRACKBALL_CLICK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config CLICK pin %d failed: %s",
                 PIN_TRACKBALL_CLICK, esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Trackball subsystem ready");
    return ESP_OK;
}

extern "C" uint8_t nanojs8_trackball_get_event(uint32_t timeout_ms) {
    if (!s_started || s_event_queue == nullptr) {
        return 0;
    }
    uint8_t ev = 0;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ?
                       portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_event_queue, &ev, ticks) == pdTRUE) {
        return ev;
    }
    return 0;
}

extern "C" uint32_t nanojs8_trackball_count(uint8_t event) {
    switch (event) {
    case NANOJS8_TRACKBALL_UP:    return s_count_up.load();
    case NANOJS8_TRACKBALL_DOWN:  return s_count_down.load();
    case NANOJS8_TRACKBALL_LEFT:  return s_count_left.load();
    case NANOJS8_TRACKBALL_RIGHT: return s_count_right.load();
    case NANOJS8_TRACKBALL_CLICK: return s_count_click.load();
    default: return 0;
    }
}
