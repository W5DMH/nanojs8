/*
 * keyboard.cpp — NanoJS8 v0.7 keyboard input implementation
 * ===========================================================
 * See keyboard.h for the public API and rationale.
 *
 * Implementation notes:
 *
 * Why one byte at a time:
 *   The C3 keyboard firmware (per LilyGO's source) exposes a single-byte
 *   register containing the most recent keypress, cleared on read. Asking
 *   for more than 1 byte just gets 0x00s after the first. So we always
 *   request exactly 1 byte.
 *
 * Why 50 ms poll:
 *   20 Hz scan is well above the maximum human typing rate (~10 chars/sec
 *   sustained), but still gentle enough on the I²C bus and the C3.
 *   Faster polling (e.g. 10 ms) wakes up the CPU more frequently for no
 *   real benefit. Slower polling (e.g. 100 ms) makes typing feel laggy.
 *
 * Why a queue rather than callback:
 *   Decouples the keyboard task from whatever consumer code is running.
 *   The UI thread (Layer 5+) can be busy redrawing or talking to the
 *   radio when a keypress arrives; the queue holds the event safely
 *   until the consumer is ready. Callbacks would force keyboard timing
 *   into the consumer's worst-case latency.
 *
 * Why "last key" is separate:
 *   The status display wants "what was the most recent key" without
 *   removing it from the queue. Storing it separately lets the queue
 *   serve real consumers while the screen shows a passive indicator.
 *
 * Error handling on read failure:
 *   The new I²C driver logs every NACK at ERROR level. We don't want
 *   that flooding the log when the C3 is briefly unresponsive (e.g.
 *   during its own internal scan), so we suppress i2c.master logs at
 *   task start. NACKs become silent ESP_ERR_TIMEOUT returns, which we
 *   simply treat as "no key right now" and continue.
 */

#include "keyboard.h"
#include "platform_tdeck.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <atomic>
#include <inttypes.h>

static const char* TAG = "kb";

// Tuning constants
static constexpr int    KB_POLL_INTERVAL_MS    = 50;
static constexpr int    KB_READ_TIMEOUT_MS     = 20;
static constexpr int    KB_TASK_STACK_BYTES    = 4096;
static constexpr int    KB_TASK_PRIORITY       = 4;
static constexpr int    KB_RETRY_DELAY_MS      = 200;

// L6b.3 hotfix1: bumped from 10 to 30 (2s → 6s) after seeing the C3
// keyboard MCU intermittently miss the initial probe window on hardware.
// The polling task tolerates NACKs and would in principle pick up the
// C3 if it wakes up later, but in practice the C3 sometimes gets stuck
// in a state where it never responds; 6s of initial retry gives slow
// cold-boots a much better chance to complete before the first probe.
//
// L7.11i: bumped from 30 to 60 (12 s total probe window). On rare cold
// boots the user reports the C3 needs even longer; the polling task
// would eventually pick it up but operator-perceived reliability is
// better if the init phase waits longer up front. 12 s × 200 ms NACKs
// is harmless from a power perspective (no inrush, no rail toggling —
// see L7.4a-fix2 lesson against POWERON cycling which triggered BOD).
//
// L7.11i.fix1: kept at 60 for now while we evaluate whether the new
// I²C preconditioning in platform_tdeck (drive SDA/SCL HIGH before
// POWERON) reduces the stuck-state frequency. If post-fix logs show
// successful boots always succeed on early attempts, we'll trim this
// back to reduce the failure-case wait time.
static constexpr int    KB_RETRY_MAX           = 60;
static constexpr uint8_t KB_NO_KEY             = 0x00;

// L7.11i (added) / L7.11i.fix1 (REMOVED): the 1000 ms pre-probe
// stabilization delay we added in L7.11i was reverted in fix1 because
// the field-test boot log on Jun 8 2026 proved it doesn't help. The
// instrumentation we added with the delay showed every successful boot
// responded on attempt 1 immediately after the delay (meaning C3 was
// already alive before the delay started — the delay was wasted), and
// every failed boot stayed dead through all 60 retries (meaning C3 was
// in a stuck state that no amount of waiting recovers). Net: the delay
// added 1 s to every boot for zero benefit. Replaced with I²C-bus
// preconditioning in platform_tdeck, which targets the actual root
// cause (C3 seeing garbage SDA/SCL at its POR moment).

// L7.11i.fix2 (added) / L7.11i.fix3 (REMOVED): auto-restart recovery
// via esp_restart() was added in fix2 hoping the GPIO reset side
// effect would collapse the peripheral rail enough to give C3 a clean
// POR on reboot. Field test Jun 8 2026 with 4 attempted restarts
// across 8 boots showed 0/4 success — the rail does NOT collapse
// meaningfully during esp_restart, and the C3 stuck-state survives
// the soft reset. Removed in fix3 because it added complexity, +30 s
// boot time on failed-kb boots, and no recovery value. The user
// reported Meshtastic and Meshcore work reliably on the same hardware
// — meaning a real firmware fix exists; we just haven't identified it
// yet. Next investigation step: get the actual Meshtastic T-Deck
// variant source and compare init sequence to ours.

// File-static state
static QueueHandle_t            s_queue = nullptr;
static i2c_master_dev_handle_t  s_dev = nullptr;
static TaskHandle_t             s_task = nullptr;
static bool                     s_started = false;
static std::atomic<uint32_t>    s_total_keys{0};
static std::atomic<uint8_t>     s_last_key{0};

// L6b.3 hotfix1: set true if the initial probe failed. The polling task
// uses this to print a one-time "keyboard recovered late" message if it
// ever does start receiving keys, so the heartbeat operator log shows
// whether the C3 came up on its own or stayed dead.
static std::atomic<bool>        s_probe_failed{false};
static std::atomic<bool>        s_recovery_logged{false};

// L6b.6: alive tracking — flips true on the FIRST successful I²C read
// (any read where the bus returned ESP_OK, key value irrelevant). This
// is a more robust signal than the legacy s_recovery_logged because it
// fires the instant the chip starts ACKing, without waiting for an
// actual keypress. The HOME UI uses this to show "Keyboard: OFFLINE"
// vs "ready" state.
static std::atomic<bool>        s_alive{false};
static std::atomic<uint32_t>    s_alive_since_ms{0};

// ---------------------------------------------------------------------------
// Initial device registration with retry
// ---------------------------------------------------------------------------

// Register 0x55 as an I²C device. The C3 firmware may not be ready
// immediately after POWERON; if the first probe fails, sleep briefly
// and try again. Returns ESP_OK once we've successfully added the device
// to the bus, ESP_ERR_NOT_FOUND if we give up.
static esp_err_t register_keyboard_device(void) {
    using nanojs8::platform::tdeck::i2c_bus;
    using nanojs8::platform::tdeck::I2C_ADDR_KEYBOARD;

    i2c_master_bus_handle_t bus = i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I²C bus not initialized — call tdeck_platform_init first");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_KEYBOARD,    // 0x55
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 },
    };

    // Add device — this doesn't actually talk to the C3 yet, just sets
    // up internal driver state. So we add it once, then probe.
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x55) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // Probe with retry. The probe is what actually causes the bus
    // transaction that we need the C3 to respond to.
    //
    // We silence the i2c.master ERROR logs during this retry loop because
    // every failed probe causes the driver to spam 3 ERROR-level lines.
    //
    // L7.11i.fix1: the 1000 ms pre-probe delay that lived here in
    // L7.11i was REMOVED — field-test logs (Jun 8 2026) proved C3
    // either responds on attempt 1 (alive when we get here) or stays
    // dead through all 60 retries (stuck state). Waiting up front
    // helped neither case. Root-cause mitigation moved to
    // platform_tdeck::init where SDA/SCL are now preconditioned HIGH
    // before POWERON energizes the peripheral rail.
    esp_log_level_t prev_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    const int64_t t_probe_start_us = esp_timer_get_time();
    bool found = false;
    int  found_attempt = 0;
    for (int attempt = 1; attempt <= KB_RETRY_MAX; ++attempt) {
        err = i2c_master_probe(bus, I2C_ADDR_KEYBOARD, 100);
        if (err == ESP_OK) {
            // Restore before logging so the success message shows
            esp_log_level_set("i2c.master", prev_level);
            const int64_t t_now_us = esp_timer_get_time();
            const int64_t probe_window_ms =
                (t_now_us - t_probe_start_us) / 1000;
            ESP_LOGI(TAG,
                "Keyboard at 0x55 responded on attempt %d/%d "
                "(probe window %" PRId64 " ms)",
                attempt, KB_RETRY_MAX, probe_window_ms);
            found = true;
            found_attempt = attempt;
            // L6b.6: a successful probe is proof of life — mark alive
            // immediately. Otherwise there's a brief window before the
            // polling task gets its first read where HOME would say
            // "kb offline" even though the C3 is clearly responsive.
            s_alive.store(true, std::memory_order_release);
            s_alive_since_ms.store(
                (uint32_t)(t_now_us / 1000),
                std::memory_order_release);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(KB_RETRY_DELAY_MS));
    }

    if (!found) {
        esp_log_level_set("i2c.master", prev_level);
        const int64_t probe_window_ms =
            (esp_timer_get_time() - t_probe_start_us) / 1000;
        ESP_LOGW(TAG,
            "Keyboard at 0x55 not responding after %d attempts over "
            "%" PRId64 " ms probe window; continuing anyway "
            "(reads return 0 until alive; polling task will pick up "
            "C3 if it ever wakes — though field-test data Jun 8 2026 "
            "showed stuck-state never recovers within a single "
            "power-on session)",
            KB_RETRY_MAX, probe_window_ms);
        s_probe_failed.store(true, std::memory_order_relaxed);
        // Don't fail — the C3 may come online later, and we'd rather
        // keep polling than abort completely. The poll loop tolerates
        // NACKs gracefully.
        //
        // L7.11i.fix2 attempted automatic recovery via esp_restart()
        // here. Removed in L7.11i.fix3 after field test (Jun 8 2026,
        // 4 restart attempts, 0/4 success) proved esp_restart doesn't
        // collapse the peripheral rail meaningfully — C3 stuck-state
        // survives the soft reset.
    } else {
        // Log attempt-of-first-success so future boot logs make it
        // obvious whether C3 is responding fast (attempt 1, no extra
        // wait needed) or slow (high attempt number, needs the wider
        // retry budget). This data drives future tuning decisions
        // for KB_RETRY_MAX.
        const int64_t probe_window_ms =
            (esp_timer_get_time() - t_probe_start_us) / 1000;
        ESP_LOGI(TAG,
            "Keyboard init OK in %" PRId64 " ms "
            "(attempt %d/%d)",
            probe_window_ms, found_attempt, KB_RETRY_MAX);
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Poll task
// ---------------------------------------------------------------------------

static void keyboard_task(void* /*arg*/) {
    // Suppress per-read ERROR logs from the I²C driver. Every NACK
    // (which happens any time the C3 doesn't have a key to give us)
    // would otherwise produce 3 lines of ERROR-level spam at 20 Hz.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    ESP_LOGI(TAG, "Keyboard task running (poll every %d ms)",
             KB_POLL_INTERVAL_MS);

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(KB_POLL_INTERVAL_MS);

    while (true) {
        uint8_t byte = 0;
        esp_err_t err = i2c_master_receive(s_dev, &byte, 1,
                                            KB_READ_TIMEOUT_MS);

        // L6b.6: any successful I²C transaction proves the C3 is alive.
        // Flip the alive flag the first time we see ESP_OK, regardless
        // of whether a key was pressed. compare_exchange_strong runs the
        // log line exactly once across the lifetime of the task.
        if (err == ESP_OK) {
            bool was = false;
            if (s_alive.compare_exchange_strong(was, true,
                                                std::memory_order_acq_rel)) {
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                s_alive_since_ms.store(now_ms, std::memory_order_release);
                ESP_LOGI(TAG, "Keyboard alive: 0x55 responded at +%" PRIu32 " ms "
                              "(initial probe %s)",
                         now_ms,
                         s_probe_failed.load(std::memory_order_relaxed)
                             ? "had failed — C3 woke up late"
                             : "had succeeded");
            }
        }

        if (err == ESP_OK && byte != KB_NO_KEY) {
            // L6b.3 hotfix1: if the initial probe failed but a key is
            // now arriving, the C3 did wake up late. Keep this log too
            // (separate from the alive log above) so we see the FIRST
            // keypress specifically — useful for diagnosing operator-
            // facing "I pressed a key and nothing happened" reports.
            if (s_probe_failed.load(std::memory_order_relaxed) &&
                !s_recovery_logged.exchange(true, std::memory_order_relaxed)) {
                ESP_LOGI(TAG, "Keyboard first keypress 0x%02X arrived "
                              "after initial probe timeout", byte);
            }

            // Got a real keypress. Update the "last key" snapshot
            // unconditionally, then try to push to the queue. If the
            // queue is full, drop silently — better than blocking.
            s_last_key.store(byte, std::memory_order_relaxed);
            s_total_keys.fetch_add(1, std::memory_order_relaxed);

            BaseType_t pushed = xQueueSend(s_queue, &byte, 0);
            if (pushed != pdTRUE) {
                // Queue full — consumer is behind. Drop the byte but
                // log occasionally so we know it's happening.
                // (Limit logging to once per ~100 drops to avoid flooding.)
                static uint32_t drop_count = 0;
                if ((++drop_count % 100) == 1) {
                    ESP_LOGW(TAG, "Queue full, dropped key 0x%02X "
                                  "(%u total drops)", byte, (unsigned)drop_count);
                }
            }
        }
        // err == ESP_ERR_TIMEOUT (NACK from C3) or other transient
        // failures: just continue. byte == 0 (no key): continue.

        vTaskDelayUntil(&last_wake, period_ticks);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_keyboard_start(void) {
    if (s_started) {
        ESP_LOGW(TAG, "Keyboard already started; ignoring");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting keyboard subsystem...");

    // 1. Register the device on the I²C bus, with retry for the C3 boot race
    esp_err_t err = register_keyboard_device();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Device registration failed: %s", esp_err_to_name(err));
        return err;
    }

    // 2. Create the event queue. Items are uint8_t (one ASCII code each).
    s_queue = xQueueCreate(NANOJS8_KB_QUEUE_DEPTH, sizeof(uint8_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed (out of memory?)");
        return ESP_FAIL;
    }

    // 3. Spawn the poll task.
    BaseType_t ok = xTaskCreate(keyboard_task, "kb_task",
                                 KB_TASK_STACK_BYTES, nullptr,
                                 KB_TASK_PRIORITY, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "Keyboard subsystem started");
    return ESP_OK;
}

extern "C" uint8_t nanojs8_keyboard_get_key(uint32_t timeout_ms) {
    if (!s_queue) return 0;
    uint8_t byte = 0;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ?
                       portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_queue, &byte, ticks) == pdTRUE) {
        return byte;
    }
    return 0;
}

extern "C" uint32_t nanojs8_keyboard_total_keys(void) {
    return s_total_keys.load(std::memory_order_relaxed);
}

extern "C" uint8_t nanojs8_keyboard_last_key(void) {
    return s_last_key.load(std::memory_order_relaxed);
}

extern "C" bool nanojs8_keyboard_is_alive(void) {
    return s_alive.load(std::memory_order_acquire);
}

extern "C" uint32_t nanojs8_keyboard_alive_since_ms(void) {
    return s_alive_since_ms.load(std::memory_order_acquire);
}
