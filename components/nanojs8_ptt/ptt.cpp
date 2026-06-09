/*
 * ptt.cpp — PTT controller implementation (L6b.4)
 * =================================================
 * See ptt.h for design notes.
 *
 * State model
 * ───────────
 *   s_keyed         : atomic<bool>  — current PTT state
 *   s_assert_us     : atomic<int64_t> — esp_timer_get_time() at last assert
 *   s_total_tx      : atomic<uint32_t> — count of release events
 *   s_wdt_trips     : atomic<uint32_t> — count of watchdog auto-releases
 *   s_active_profile: const nanojs8_radio_profile_t * — current profile
 *
 * Watchdog task
 * ─────────────
 * A dedicated FreeRTOS task wakes every 250 ms and checks whether
 * PTT has been asserted longer than NANOJS8_PTT_WATCHDOG_MS. If so,
 * it force-releases by calling nanojs8_ptt_set(false). The task is
 * lightweight (~1 KB stack, low priority) — 250 ms granularity means
 * worst-case overshoot is 0.25 s above the 20 s limit, well within
 * radio finals' thermal headroom.
 *
 * Why a task rather than esp_timer? Driving the USB serial PTT line
 * makes a USB control transfer which can block briefly; doing that
 * from the esp_timer dispatcher task could delay other timers. A
 * dedicated task with its own scheduling is cleaner.
 *
 * License: GPL-3.0
 */

#include "ptt.h"
#include "radio.h"
#include "usb_serial.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <string.h>

static const char* TAG = "ptt";

namespace {

constexpr int   WDT_POLL_INTERVAL_MS = 250;
constexpr int   WDT_TASK_STACK_BYTES = 2048;
constexpr int   WDT_TASK_PRIORITY    = 3;

std::atomic<bool>     s_started{false};
std::atomic<bool>     s_keyed{false};
std::atomic<int64_t>  s_assert_us{0};
std::atomic<uint32_t> s_total_tx{0};
std::atomic<uint32_t> s_wdt_trips{0};

// L7.11f-fix2: per-burst watchdog ceiling override. 0 means "use the
// default NANOJS8_PTT_WATCHDOG_MS". Non-zero means the caller has
// explicitly bumped the ceiling for the next PTT-asserted window; this
// is consumed (reset to 0) on the next ptt_set(false) call so the
// system always reverts to safe-by-default. See ptt.h API doc for the
// MicroJS8 "continuous PTT across multi-frame burst" rationale.
std::atomic<uint32_t> s_burst_limit_ms{0};

// Active profile pointer. Set in start() and apply_profile(). Reads are
// done without locking — the pointer is updated atomically (single
// machine-word write) and points into the static registry array, so
// the pointee is always valid for the program's lifetime.
std::atomic<const nanojs8_radio_profile_t *> s_profile{nullptr};

// One-time-warning state for "PTT requested but profile has no PTT
// mechanism" — avoids log spam if the modem layer keys repeatedly.
std::atomic<bool> s_warned_no_ptt{false};

TaskHandle_t s_wdt_task = nullptr;

// Push the profile's PTT mechanism down to the serial layer. Called
// from start() and apply_profile().
void configure_serial_line(const nanojs8_radio_profile_t *p) {
    if (!p) return;
    switch (p->ptt) {
    case NANOJS8_RADIO_PTT_RTS:
        nanojs8_serial_ptt_line_set(NANOJS8_SERIAL_PTT_RTS);
        ESP_LOGI(TAG, "Profile '%s' uses RTS for PTT", p->id);
        break;
    case NANOJS8_RADIO_PTT_DTR:
        nanojs8_serial_ptt_line_set(NANOJS8_SERIAL_PTT_DTR);
        ESP_LOGI(TAG, "Profile '%s' uses DTR for PTT", p->id);
        break;
    case NANOJS8_RADIO_PTT_NONE:
        ESP_LOGI(TAG, "Profile '%s' has no PTT mechanism — ptt_set() will no-op",
                 p->id);
        break;
    case NANOJS8_RADIO_PTT_CAT:
        // L6b.4 doesn't have any profile using CAT-only PTT yet; reserve
        // for L6b.5+ once we have a CAT command builder.
        ESP_LOGW(TAG, "Profile '%s' wants CAT PTT but L6b.4 doesn't "
                      "implement it; falling back to RTS so the operator "
                      "isn't dead in the water", p->id);
        nanojs8_serial_ptt_line_set(NANOJS8_SERIAL_PTT_RTS);
        break;
    }
}

// Watchdog poll task. Sleeps 250 ms, then checks elapsed time since the
// last assert. If past the limit, force-release. We re-arm s_keyed and
// the counter inside ptt_set() so the next assert starts a fresh clock.
//
// L7.11f-fix2: the limit is now runtime-read from s_burst_limit_ms (0 =
// use NANOJS8_PTT_WATCHDOG_MS default). Multi-frame TX callers bump
// this via nanojs8_ptt_set_burst_watchdog_ms() and the limit is auto-
// reset on the next release in ptt_set(false).
void wdt_task(void* /*arg*/) {
    ESP_LOGI(TAG, "Watchdog task running (poll %d ms, default limit %d ms; "
                  "burst-limit override starts cleared)",
             WDT_POLL_INTERVAL_MS, NANOJS8_PTT_WATCHDOG_MS);
    const TickType_t period = pdMS_TO_TICKS(WDT_POLL_INTERVAL_MS);
    while (true) {
        vTaskDelay(period);
        if (!s_keyed.load(std::memory_order_relaxed)) continue;
        int64_t assert_us = s_assert_us.load(std::memory_order_relaxed);
        int64_t now_us    = esp_timer_get_time();
        int64_t elapsed_ms = (now_us - assert_us) / 1000;
        uint32_t burst_limit = s_burst_limit_ms.load(std::memory_order_relaxed);
        uint32_t limit_ms = (burst_limit != 0) ? burst_limit
                                               : (uint32_t)NANOJS8_PTT_WATCHDOG_MS;
        if (elapsed_ms > (int64_t)limit_ms) {
            ESP_LOGW(TAG, "Watchdog tripped: PTT was asserted for %lld ms "
                          "(limit %u ms%s). Force-releasing.",
                     (long long)elapsed_ms, (unsigned)limit_ms,
                     (burst_limit != 0) ? " — bumped from default" : "");
            s_wdt_trips.fetch_add(1, std::memory_order_relaxed);
            // Call ptt_set(false) — same path as a normal release, so
            // serial line goes low, counters increment, and the burst-
            // limit override is consumed.
            nanojs8_ptt_set(false);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_ptt_start(void) {
    if (s_started.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "Already started; ignoring");
        return ESP_OK;
    }

    const nanojs8_radio_profile_t *p = nanojs8_radio_get_active();
    if (!p) {
        // Should never happen — get_active() always returns at least the
        // default. But guard against future bugs that could return null.
        ESP_LOGE(TAG, "No active radio profile available; refusing to start");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Starting PTT subsystem with profile: %s (%s) "
                  "[ptt_on=%ums ptt_off=%ums]",
             p->id, p->display_name,
             (unsigned)p->ptt_on_delay_ms, (unsigned)p->ptt_off_delay_ms);

    s_profile.store(p, std::memory_order_relaxed);
    configure_serial_line(p);

    // Make sure PTT is released before the watchdog task starts. This
    // covers the case where a previous run died with PTT still high
    // (unlikely with the watchdog in place, but cheap insurance).
    nanojs8_serial_ptt_set(false);
    s_keyed.store(false, std::memory_order_relaxed);

    BaseType_t ok = xTaskCreatePinnedToCore(
        wdt_task, "ptt_wdt", WDT_TASK_STACK_BYTES, nullptr,
        WDT_TASK_PRIORITY, &s_wdt_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn watchdog task");
        return ESP_ERR_NO_MEM;
    }

    s_started.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "PTT subsystem started");
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_ptt_apply_profile(
    const nanojs8_radio_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    if (!s_started.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "apply_profile before start; remembering for boot");
        s_profile.store(profile, std::memory_order_relaxed);
        return ESP_ERR_INVALID_STATE;
    }

    // If we're currently keyed, drop PTT before switching lines.
    // Otherwise the old line could be left asserted if the new profile
    // uses a different line.
    if (s_keyed.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "Profile change while PTT is keyed — releasing first");
        nanojs8_ptt_set(false);
    }

    s_profile.store(profile, std::memory_order_release);
    configure_serial_line(profile);
    s_warned_no_ptt.store(false, std::memory_order_relaxed);
    ESP_LOGI(TAG, "Switched to profile: %s (%s) "
                  "[ptt_on=%ums ptt_off=%ums]",
             profile->id, profile->display_name,
             (unsigned)profile->ptt_on_delay_ms,
             (unsigned)profile->ptt_off_delay_ms);
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_ptt_set(bool transmitting) {
    if (!s_started.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    const nanojs8_radio_profile_t *p =
        s_profile.load(std::memory_order_acquire);
    if (!p) return ESP_ERR_INVALID_STATE;

    if (p->ptt == NANOJS8_RADIO_PTT_NONE) {
        if (!s_warned_no_ptt.exchange(true, std::memory_order_relaxed)) {
            ESP_LOGW(TAG, "ptt_set(%d) ignored: active profile '%s' has no PTT",
                     (int)transmitting, p->id);
        }
        return ESP_OK;
    }

    bool was_keyed = s_keyed.load(std::memory_order_relaxed);

    if (transmitting) {
        // Drive line high, mark keyed, record timestamp.
        esp_err_t err = nanojs8_serial_ptt_set(true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Serial ptt_set(true) failed: %s — radio may not be "
                          "connected", esp_err_to_name(err));
            return err;
        }
        s_assert_us.store(esp_timer_get_time(), std::memory_order_relaxed);
        s_keyed.store(true, std::memory_order_release);
        if (!was_keyed) {
            ESP_LOGI(TAG, "PTT ASSERTED (profile '%s')", p->id);
        }
    } else {
        // Release.
        esp_err_t err = nanojs8_serial_ptt_set(false);
        if (err != ESP_OK) {
            // Even on error, mark released — we don't want stale "still
            // keyed" state if the radio is gone.
            ESP_LOGW(TAG, "Serial ptt_set(false) failed: %s — marking "
                          "released locally anyway", esp_err_to_name(err));
        }
        if (was_keyed) {
            int64_t assert_us = s_assert_us.load(std::memory_order_relaxed);
            int64_t held_ms = (esp_timer_get_time() - assert_us) / 1000;
            s_total_tx.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGI(TAG, "PTT released after %lld ms (total tx: %u)",
                     (long long)held_ms,
                     (unsigned)s_total_tx.load(std::memory_order_relaxed));
        }
        s_keyed.store(false, std::memory_order_release);

        // L7.11f-fix2: consume any burst-limit override. The next assert
        // will use the default NANOJS8_PTT_WATCHDOG_MS unless a caller
        // explicitly bumps it again. This is the safety belt: even if
        // we got here via the watchdog tripping mid-burst (caller hung
        // after bumping), the limit reverts to safe-by-default and a
        // subsequent legitimate single-frame TX is not exposed to the
        // bumped ceiling.
        uint32_t prev_burst =
            s_burst_limit_ms.exchange(0, std::memory_order_relaxed);
        if (prev_burst != 0) {
            ESP_LOGI(TAG, "Burst watchdog limit consumed (was %u ms) — "
                          "restored to default %d ms",
                     (unsigned)prev_burst, NANOJS8_PTT_WATCHDOG_MS);
        }
    }
    return ESP_OK;
}

extern "C" bool nanojs8_ptt_is_keyed(void) {
    return s_keyed.load(std::memory_order_relaxed);
}

extern "C" uint64_t nanojs8_ptt_keyed_ms(void) {
    if (!s_keyed.load(std::memory_order_relaxed)) return 0;
    int64_t assert_us = s_assert_us.load(std::memory_order_relaxed);
    int64_t now_us    = esp_timer_get_time();
    if (now_us < assert_us) return 0;  // clock skew sanity
    return (uint64_t)((now_us - assert_us) / 1000);
}

extern "C" uint32_t nanojs8_ptt_total_tx(void) {
    return s_total_tx.load(std::memory_order_relaxed);
}

extern "C" uint32_t nanojs8_ptt_watchdog_trips(void) {
    return s_wdt_trips.load(std::memory_order_relaxed);
}

// L7.11f-fix2: bump the PTT watchdog ceiling for the NEXT release-gated
// window. See ptt.h for the full contract; the short version is that
// limit_ms persists until the next ptt_set(false) call (or watchdog
// trip), then auto-resets to 0. limit_ms below NANOJS8_PTT_WATCHDOG_MS
// is rejected — callers can extend the safety net, not shorten it.
extern "C" void nanojs8_ptt_set_burst_watchdog_ms(uint32_t limit_ms) {
    if (limit_ms == 0) {
        // Explicit clear path — e.g. caller aborted between bump and
        // PTT key. Idempotent.
        uint32_t prev =
            s_burst_limit_ms.exchange(0, std::memory_order_relaxed);
        if (prev != 0) {
            ESP_LOGI(TAG, "Burst watchdog limit cleared by caller (was %u ms)",
                     (unsigned)prev);
        }
        return;
    }
    if (limit_ms < (uint32_t)NANOJS8_PTT_WATCHDOG_MS) {
        ESP_LOGW(TAG, "Burst watchdog limit %u ms rejected — below default "
                      "%d ms (callers can extend, not shorten the safety net)",
                 (unsigned)limit_ms, NANOJS8_PTT_WATCHDOG_MS);
        return;
    }
    s_burst_limit_ms.store(limit_ms, std::memory_order_relaxed);
    ESP_LOGI(TAG, "Burst watchdog armed: next assert will use %u ms limit "
                  "(default is %d ms; auto-resets on next release)",
             (unsigned)limit_ms, NANOJS8_PTT_WATCHDOG_MS);
}
