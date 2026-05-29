// NanoJS8 — Power management subsystem implementation.

#include "power_manager.h"

#include <atomic>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <M5Cardputer.h>

#include "config_store.h"
#include "radio_service.h"

namespace nanojs8 {
namespace power {

static const char* TAG = "power";

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Battery sample cadence. The fuel gauge doesn't change fast; 3 s keeps
// the rolling average responsive without burning CPU.
static constexpr uint32_t SAMPLE_INTERVAL_MS = 3000;

// Rolling-average window. 8 samples × 3 s = 24 s of smoothing. Enough to
// kill the jitter from load transients (e.g. display redraw spikes)
// without lagging real charge/discharge trends noticeably.
static constexpr int AVG_WINDOW = 8;

// Level thresholds (percent).
static constexpr int LOW_PCT      = 20;
static constexpr int CRITICAL_PCT = 10;

// Full and dim brightness as raw backlight values (M5GFX uses 0-255).
static constexpr uint8_t BRIGHTNESS_FULL = 200;   // matches normal UI default

// Monitor task config.
static constexpr uint32_t MONITOR_STACK    = 3072;
static constexpr UBaseType_t MONITOR_PRIO  = 3;
static constexpr BaseType_t MONITOR_CORE   = 0;   // co-locate with UI on core 0

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static std::atomic<bool>        s_inited{false};
static std::atomic<int>         s_battery_pct{-1};
static std::atomic<int>         s_battery_mv{-1};
static std::atomic<Level>       s_level{Level::NORMAL};
static std::atomic<ScreenState> s_screen{ScreenState::FULL};        // actually-applied state
static std::atomic<ScreenState> s_desired_screen{ScreenState::FULL};// requested by monitor/charge/activity
static std::atomic<bool>        s_charge_mode{false};
static std::atomic<int64_t>     s_last_activity_us{0};
static std::atomic<bool>        s_critical_radio_shed{false};  // latched: did we stop radio for critical?

static Settings                 s_settings{120, 300, 30};  // defaults; overwritten from NVS in init()

// Rolling average ring.
static int  s_mv_ring[AVG_WINDOW] = {0};
static int  s_ring_count = 0;
static int  s_ring_idx   = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int64_t now_us() { return esp_timer_get_time(); }

static uint8_t pct_to_brightness(uint8_t pct) {
    // Map 0-100 to 0-255, clamped.
    if (pct > 100) pct = 100;
    return (uint8_t)((int)pct * 255 / 100);
}

static void apply_screen_full() {
    M5Cardputer.Display.wakeup();
    M5Cardputer.Display.setBrightness(BRIGHTNESS_FULL);
}

static void apply_screen_dim() {
    M5Cardputer.Display.setBrightness(pct_to_brightness(s_settings.dim_brightness));
}

static void apply_screen_blank() {
    M5Cardputer.Display.setBrightness(0);
}

static void apply_screen_charge() {
    // Charge mode: backlight off AND panel asleep, for maximum power
    // saving so the most charge current reaches the battery.
    M5Cardputer.Display.setBrightness(0);
    M5Cardputer.Display.sleep();
}

static int read_battery_mv_raw() {
    return M5Cardputer.Power.getBatteryVoltage();  // mV
}

static int read_battery_pct_raw() {
    return M5Cardputer.Power.getBatteryLevel();    // 0-100
}

static int rolling_avg_mv(int new_mv) {
    s_mv_ring[s_ring_idx] = new_mv;
    s_ring_idx = (s_ring_idx + 1) % AVG_WINDOW;
    if (s_ring_count < AVG_WINDOW) s_ring_count++;
    long sum = 0;
    for (int i = 0; i < s_ring_count; ++i) sum += s_mv_ring[i];
    return (int)(sum / s_ring_count);
}

static Level classify(int pct) {
    if (pct <= CRITICAL_PCT) return Level::CRITICAL;
    if (pct <= LOW_PCT)      return Level::LOW;
    return Level::NORMAL;
}

// ---------------------------------------------------------------------------
// Monitor task
// ---------------------------------------------------------------------------

static void monitor_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "Power monitor task started on core %d", xPortGetCoreID());

    // Prime the activity timer so we don't instantly dim at boot.
    s_last_activity_us.store(now_us(), std::memory_order_release);

    Level last_logged_level = Level::NORMAL;

    while (true) {
        // ---- Battery sampling ----
        const int raw_mv  = read_battery_mv_raw();
        const int raw_pct = read_battery_pct_raw();
        if (raw_mv > 0) {
            const int avg_mv = rolling_avg_mv(raw_mv);
            s_battery_mv.store(avg_mv, std::memory_order_release);
        }
        if (raw_pct >= 0 && raw_pct <= 100) {
            s_battery_pct.store(raw_pct, std::memory_order_release);
        }

        const int pct = s_battery_pct.load(std::memory_order_acquire);
        const Level lvl = classify(pct);
        s_level.store(lvl, std::memory_order_release);

        // ---- Low/critical handling ----
        if (lvl != last_logged_level) {
            switch (lvl) {
                case Level::LOW:
                    ESP_LOGW(TAG, "Battery LOW: %d%% (%d mV)", pct,
                             s_battery_mv.load(std::memory_order_acquire));
                    break;
                case Level::CRITICAL:
                    ESP_LOGE(TAG, "Battery CRITICAL: %d%% — shedding load "
                                  "(stopping radio service)", pct);
                    break;
                case Level::NORMAL:
                    ESP_LOGI(TAG, "Battery back to NORMAL: %d%%", pct);
                    break;
            }
            last_logged_level = lvl;
        }

        // Critical → shed load once (stop radio). Latched so we don't
        // spam stop() every sample. Cleared when we climb out of critical.
        if (lvl == Level::CRITICAL) {
            if (!s_critical_radio_shed.load(std::memory_order_acquire)) {
                if (nanojs8::radio::status() != nanojs8::radio::Status::IDLE) {
                    ESP_LOGW(TAG, "Critical battery: stopping radio service to conserve power");
                    nanojs8::radio::stop();
                }
                s_critical_radio_shed.store(true, std::memory_order_release);
            }
        } else {
            s_critical_radio_shed.store(false, std::memory_order_release);
        }

        // ---- Idle screen management ----
        // Skipped entirely when in charge mode (charge mode owns the
        // screen). Idle dim/blank operates even during radio service —
        // it's display-only and never touches the radio; any keypress
        // restores full brightness, so it won't blank mid-interaction.
        if (!s_charge_mode.load(std::memory_order_acquire)) {
            const int64_t idle_us = now_us() - s_last_activity_us.load(std::memory_order_acquire);
            const uint32_t idle_sec = (uint32_t)(idle_us / 1000000);

            const uint16_t dim_at = s_settings.idle_dim_sec;
            const uint16_t off_at = s_settings.idle_off_sec;

            ScreenState desired = ScreenState::FULL;
            if (off_at > 0 && idle_sec >= off_at) {
                desired = ScreenState::BLANKED;
            } else if (dim_at > 0 && idle_sec >= dim_at) {
                desired = ScreenState::DIMMED;
            }

            // IMPORTANT: do NOT touch the display from this task. M5GFX
            // is not thread-safe and the UI task on the same core owns
            // the display bus. We only record the DESIRED state here;
            // the UI task applies it via apply_pending_screen_change()
            // during its own tick, serializing all display access.
            s_desired_screen.store(desired, std::memory_order_release);
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(void) {
    if (s_inited.exchange(true)) {
        return;  // idempotent
    }

    // Load settings from NVS config.
    const Config& cfg = nanojs8::config::current();
    s_settings.idle_dim_sec  = cfg.idle_dim_sec;
    s_settings.idle_off_sec  = cfg.idle_off_sec;
    s_settings.dim_brightness = cfg.dim_brightness;

    ESP_LOGI(TAG, "Power init: idle_dim=%us idle_off=%us dim_bright=%u%%",
             (unsigned)s_settings.idle_dim_sec,
             (unsigned)s_settings.idle_off_sec,
             (unsigned)s_settings.dim_brightness);

    s_last_activity_us.store(now_us(), std::memory_order_release);
    s_screen.store(ScreenState::FULL, std::memory_order_release);

    xTaskCreatePinnedToCore(monitor_task, "power_mon",
                            MONITOR_STACK, nullptr,
                            MONITOR_PRIO, nullptr, MONITOR_CORE);
}

int battery_pct(void) { return s_battery_pct.load(std::memory_order_acquire); }
int battery_mv(void)  { return s_battery_mv.load(std::memory_order_acquire); }
Level level(void)     { return s_level.load(std::memory_order_acquire); }

void snapshot(Snapshot* out) {
    if (!out) return;
    out->battery_pct    = s_battery_pct.load(std::memory_order_acquire);
    out->battery_mv     = s_battery_mv.load(std::memory_order_acquire);
    out->level          = s_level.load(std::memory_order_acquire);
    out->screen_state   = s_screen.load(std::memory_order_acquire);
    out->in_charge_mode = s_charge_mode.load(std::memory_order_acquire);
    const int64_t idle_us = now_us() - s_last_activity_us.load(std::memory_order_acquire);
    out->idle_sec = (uint32_t)(idle_us / 1000000);
}

void enter_charge_mode(void) {
    if (s_charge_mode.exchange(true)) {
        return;  // already in charge mode
    }
    ESP_LOGI(TAG, "Entering charge mode (battery %d%%, %d mV)",
             battery_pct(), battery_mv());

    // Shed load: stop the radio service if running. Charging and USB-host
    // are mutually exclusive on the single USB-C port anyway.
    if (nanojs8::radio::status() != nanojs8::radio::Status::IDLE) {
        ESP_LOGI(TAG, "Charge mode: stopping radio service");
        nanojs8::radio::stop();
    }

    // Request CHARGE screen state. The UI task applies the actual panel
    // sleep + backlight off via apply_pending_screen_change() — we never
    // touch M5GFX from here (could be called from console task or UI task;
    // serializing display access through the UI task avoids bus races).
    s_desired_screen.store(ScreenState::CHARGE, std::memory_order_release);

    ESP_LOGI(TAG, "Charge mode ACTIVE — screen off, CPU full clock (console live). "
                  "Press any key on the Cardputer or send `charge off` to exit.");
}

void exit_charge_mode(void) {
    if (!s_charge_mode.exchange(false)) {
        return;  // wasn't in charge mode
    }
    s_last_activity_us.store(now_us(), std::memory_order_release);
    s_desired_screen.store(ScreenState::FULL, std::memory_order_release);
    ESP_LOGI(TAG, "Charge mode EXIT (battery %d%%, %d mV)",
             battery_pct(), battery_mv());
}

bool in_charge_mode(void) {
    return s_charge_mode.load(std::memory_order_acquire);
}

void notify_activity(void) {
    s_last_activity_us.store(now_us(), std::memory_order_release);

    // Exit charge mode on any activity.
    if (s_charge_mode.load(std::memory_order_acquire)) {
        exit_charge_mode();
        return;
    }

    // Request wake to FULL. Actual brightness restore happens in the UI
    // task via apply_pending_screen_change().
    s_desired_screen.store(ScreenState::FULL, std::memory_order_release);
}

void apply_pending_screen_change(void) {
    // Called ONLY from the UI task, which owns the display bus. Applies
    // any pending screen-state transition. This is the single place that
    // touches M5GFX brightness/sleep, so there's no cross-task bus race.
    const ScreenState desired = s_desired_screen.load(std::memory_order_acquire);
    const ScreenState cur     = s_screen.load(std::memory_order_acquire);
    if (desired == cur) {
        return;
    }
    switch (desired) {
        case ScreenState::FULL:    apply_screen_full();   break;
        case ScreenState::DIMMED:  apply_screen_dim();    break;
        case ScreenState::BLANKED: apply_screen_blank();  break;
        case ScreenState::CHARGE:  apply_screen_charge(); break;
    }
    s_screen.store(desired, std::memory_order_release);
    ESP_LOGD(TAG, "Screen applied -> %d", (int)desired);
}

bool ui_rendering_suppressed(void) {
    // Suppress based on the ACTUAL applied state (after the UI task has
    // had a chance to apply it) so we don't skip the very draw that would
    // happen alongside the transition. Use desired when it's a suppress
    // state so we stop drawing immediately on request.
    const ScreenState desired = s_desired_screen.load(std::memory_order_acquire);
    return desired == ScreenState::BLANKED || desired == ScreenState::CHARGE;
}

const Settings& settings(void) { return s_settings; }

void set_idle_dim_sec(uint16_t sec) {
    s_settings.idle_dim_sec = sec;
    nanojs8::config::set_idle_dim_sec(sec);
    nanojs8::config::save();
}
void set_idle_off_sec(uint16_t sec) {
    s_settings.idle_off_sec = sec;
    nanojs8::config::set_idle_off_sec(sec);
    nanojs8::config::save();
}
void set_dim_brightness(uint8_t pct) {
    s_settings.dim_brightness = pct;
    nanojs8::config::set_dim_brightness(pct);
    nanojs8::config::save();
}

} // namespace power
} // namespace nanojs8
