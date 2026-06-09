/*
 * time_source.cpp — Manual UTC clock implementation (Layer 7.0)
 * =====================================================
 * Storage model:
 *
 *   s_anchor_us       : esp_timer_get_time() at the moment the operator
 *                       last called set_utc(). 0 if never set.
 *   s_anchor_today_s  : (hour*3600 + minute*60 + second) — the
 *                       "seconds since midnight" the operator entered.
 *
 *   Current seconds-since-midnight =
 *     (s_anchor_today_s + (now_us - s_anchor_us) / 1_000_000) % 86400
 *
 * Both atomics are written together inside set_utc, read separately
 * elsewhere. There IS a window where one updates and the other hasn't,
 * but readers are tolerant: the worst case is one read returning a
 * time that's off by however long set_utc took to execute (microseconds).
 * For a wall clock this is acceptable. If we ever care, we'd pack both
 * into a 128-bit atomic — overkill for now.
 *
 * License: GPL-3.0
 */

#include "time_source.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <atomic>

static const char *TAG = "time";

namespace {

// Microseconds-per-second factor for esp_timer math.
constexpr uint64_t US_PER_SEC  = 1000000ULL;
constexpr uint32_t MS_PER_SEC  = 1000U;
constexpr uint32_t SEC_PER_DAY = 86400U;

// Anchor state. s_anchor_us == 0 means "not set"; we never set
// esp_timer_get_time() to 0 in practice (it's monotonic from boot
// and the very first reading is well past 0 by the time SETUP could
// possibly fire), so 0 is a safe sentinel.
std::atomic<uint64_t> s_anchor_us{0};
std::atomic<uint32_t> s_anchor_today_s{0};

// L7.14: which subsystem set the anchor most recently? Atomic so
// the GPS reader task and the UI render task can read it without
// synchronization. Stored as int so std::atomic stays trivially-
// constructible (the enum cast is at the access points).
std::atomic<int> s_source{(int)NANOJS8_TIME_SOURCE_NONE};

} // namespace

extern "C" esp_err_t nanojs8_time_start(void) {
    // Reset to "not set" — operator must enter UTC every session.
    // Done explicitly here (not as a static init) so a future
    // hot-reload path will also clear stale anchors.
    s_anchor_us.store(0, std::memory_order_release);
    s_anchor_today_s.store(0, std::memory_order_release);
    s_source.store((int)NANOJS8_TIME_SOURCE_NONE,
                   std::memory_order_release);
    ESP_LOGI(TAG, "Time subsystem started — UTC not yet set "
                  "(operator must enter via SETUP)");
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_time_set_utc(uint8_t hour,
                                          uint8_t minute,
                                          uint8_t second) {
    if (hour >= 24 || minute >= 60 || second >= 60) {
        ESP_LOGW(TAG, "set_utc(%u:%u:%u) out of range",
                 (unsigned)hour, (unsigned)minute, (unsigned)second);
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t today_s = (uint32_t)hour * 3600U +
                       (uint32_t)minute * 60U +
                       (uint32_t)second;

    // Order matters: write today_s FIRST, anchor_us SECOND. A reader
    // that races could see the new today_s with the old anchor_us
    // (would compute a too-large delta and roll modulo 86400), or
    // the old today_s with the new anchor_us (would compute too-small
    // delta). Both produce a brief glitch but recover within microseconds.
    // For a wall-clock that the user manually entered, this is fine.
    s_anchor_today_s.store(today_s, std::memory_order_release);
    s_anchor_us.store(now_us, std::memory_order_release);
    s_source.store((int)NANOJS8_TIME_SOURCE_MANUAL,
                   std::memory_order_release);

    ESP_LOGI(TAG, "UTC set to %02u:%02u:%02u (anchor=%llu us, source=MANUAL)",
             (unsigned)hour, (unsigned)minute, (unsigned)second,
             (unsigned long long)now_us);
    return ESP_OK;
}

// L7.14: GPS-sourced set. Same atomic anchor mechanism, marks source
// as GPS. Per the project policy "GPS time is always preferred over
// manual time regardless of offset" — this overwrites unconditionally.
// Called from the GPS reader task on initial fix and on the 60-second
// re-sync tick.
extern "C" esp_err_t nanojs8_time_set_utc_from_gps(uint8_t hour,
                                                   uint8_t minute,
                                                   uint8_t second) {
    if (hour >= 24 || minute >= 60 || second >= 60) {
        ESP_LOGW(TAG, "set_utc_from_gps(%u:%u:%u) out of range",
                 (unsigned)hour, (unsigned)minute, (unsigned)second);
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t today_s = (uint32_t)hour * 3600U +
                       (uint32_t)minute * 60U +
                       (uint32_t)second;
    s_anchor_today_s.store(today_s, std::memory_order_release);
    s_anchor_us.store(now_us, std::memory_order_release);
    s_source.store((int)NANOJS8_TIME_SOURCE_GPS,
                   std::memory_order_release);
    // NOTE: This may run after the runtime-console handover (when
    // CONFIG_NANOJS8_GPS_ENABLED=y), in which case the ESP_LOGI below
    // becomes a no-op via the global log level. That's fine — the
    // HOME GPS row shows the same info on screen.
    ESP_LOGI(TAG, "UTC set to %02u:%02u:%02u (anchor=%llu us, source=GPS)",
             (unsigned)hour, (unsigned)minute, (unsigned)second,
             (unsigned long long)now_us);
    return ESP_OK;
}

extern "C" nanojs8_time_source_t nanojs8_time_get_source(void) {
    return (nanojs8_time_source_t)s_source.load(std::memory_order_acquire);
}

extern "C" bool nanojs8_time_is_set(void) {
    return s_anchor_us.load(std::memory_order_acquire) != 0;
}

extern "C" bool nanojs8_time_get_utc(uint8_t *out_hour,
                                     uint8_t *out_minute,
                                     uint8_t *out_second) {
    uint64_t anchor_us = s_anchor_us.load(std::memory_order_acquire);
    if (anchor_us == 0) {
        return false;
    }
    uint64_t now_us   = (uint64_t)esp_timer_get_time();
    uint32_t today_s  = s_anchor_today_s.load(std::memory_order_acquire);
    uint64_t delta_us = (now_us >= anchor_us) ? (now_us - anchor_us) : 0;
    uint32_t delta_s  = (uint32_t)(delta_us / US_PER_SEC);
    uint32_t cur_s    = (today_s + delta_s) % SEC_PER_DAY;

    if (out_hour)   *out_hour   = (uint8_t)(cur_s / 3600U);
    if (out_minute) *out_minute = (uint8_t)((cur_s / 60U) % 60U);
    if (out_second) *out_second = (uint8_t)(cur_s % 60U);
    return true;
}

extern "C" uint32_t nanojs8_time_seconds_today(void) {
    uint64_t anchor_us = s_anchor_us.load(std::memory_order_acquire);
    if (anchor_us == 0) {
        return 0;
    }
    uint64_t now_us   = (uint64_t)esp_timer_get_time();
    uint32_t today_s  = s_anchor_today_s.load(std::memory_order_acquire);
    uint64_t delta_us = (now_us >= anchor_us) ? (now_us - anchor_us) : 0;
    uint32_t delta_s  = (uint32_t)(delta_us / US_PER_SEC);
    return (today_s + delta_s) % SEC_PER_DAY;
}

// L7.1: milliseconds-since-midnight. Same derivation as seconds_today
// but at 1000× finer resolution. Critical for slot-boundary timing —
// at 1-second resolution we'd be ±500 ms off the actual boundary, way
// outside JS8's ±100 ms slot tolerance.
extern "C" uint32_t nanojs8_time_millis_today(void) {
    uint64_t anchor_us = s_anchor_us.load(std::memory_order_acquire);
    if (anchor_us == 0) {
        return 0;
    }
    uint64_t now_us   = (uint64_t)esp_timer_get_time();
    uint32_t today_s  = s_anchor_today_s.load(std::memory_order_acquire);
    uint64_t delta_us = (now_us >= anchor_us) ? (now_us - anchor_us) : 0;
    // Compose ms-since-midnight from (today_s * 1000) + (delta_us / 1000).
    // Use 64-bit math: today_s up to 86_399 × 1000 = 86.4M fits in uint64_t.
    uint64_t today_ms = (uint64_t)today_s * 1000ULL + (delta_us / 1000ULL);
    return (uint32_t)(today_ms % (SEC_PER_DAY * 1000ULL));
}

extern "C" uint32_t nanojs8_time_age_ms(void) {
    uint64_t anchor_us = s_anchor_us.load(std::memory_order_acquire);
    if (anchor_us == 0) {
        return 0;
    }
    uint64_t now_us   = (uint64_t)esp_timer_get_time();
    uint64_t delta_us = (now_us >= anchor_us) ? (now_us - anchor_us) : 0;
    return (uint32_t)(delta_us / (US_PER_SEC / MS_PER_SEC));
}
