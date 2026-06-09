/*
 * radio.cpp — radio profile registry implementation (L6b.5)
 * ===========================================================
 * See radio.h for the public API and design notes.
 *
 * The registry is a static array initialized at compile time. The
 * first entry is the default profile (used as a fallback when the
 * stored radio_id doesn't match any registered profile).
 *
 * L6b.5 profile set:
 *   [0] digirig-rts-only    — default, RTS PTT only
 *   [1] xiegu-g90-digirig   — RTS PTT + Icom CI-V CAT at 19200, 0x70
 *
 * The L6b.4 trusdx-ts480 and ts480 profiles were removed because:
 *   - (tr)uSDX has no CAT path through DigiRig (the TRRS jack carries
 *     audio + PTT only, never CAT serial). Direct USB-CAT on (tr)uSDX
 *     firmware 2.00x is documented-unreliable.
 *   - The Kenwood TS-480 radio itself is still a valid future target
 *     but speaks ASCII CAT (FA00007078000;) — a different code path
 *     from CI-V. Deferred until we have one to test against.
 *
 * Stale NVS migration: users with radio_id="ts480" or "trusdx-ts480"
 * from a previous build hit the fallback path in get_active(), get
 * the default profile (digirig-rts-only), and see one WARN line in
 * the log. Their NVS isn't rewritten until they next visit SETUP and
 * commit a profile change, which is the safer behavior — no silent
 * config mutation.
 *
 * License: GPL-3.0
 */

#include "radio.h"
#include "config.h"

#include "esp_log.h"
#include <string.h>
#include <atomic>

static const char* TAG = "radio";

namespace {

// ── Registry ───────────────────────────────────────────────────────
//
// digirig-rts-only is FIRST and therefore the default. It is the
// safest profile: no CAT requirements, no protocol assumptions, just
// hardware PTT via RTS. Works with any radio that has a PTT input
// even if we know nothing else about it.
//
// xiegu-g90-digirig is the canonical CAT-equipped profile, matching
// MicroJS8's id and field naming so operators moving between projects
// don't have to learn a new vocabulary. PTT_ON / PTT_OFF delays come
// from MicroJS8's empirical tuning (300/200 ms for G90, settled value).

const nanojs8_radio_profile_t s_profiles[] = {
    {
        .id                  = "digirig-rts-only",
        .display_name        = "DigiRig RTS",
        .description         = "Hardware PTT only, no CAT, freq is informational",
        .ptt                 = NANOJS8_RADIO_PTT_RTS,
        // FM walkies and (tr)uSDX have slow PTT chains; 300/200 ms
        // covers the optoisolator settle and any radio-side mode
        // switching. Matches MicroJS8's tuning for this profile.
        .ptt_on_delay_ms     = 300,
        .ptt_off_delay_ms    = 200,
        .cat                 = NANOJS8_RADIO_CAT_NONE,
        .cat_baud            = 0,
        .can_set_freq        = false,
        .cat_civ_radio_addr  = 0,   // unused when cat == CAT_NONE
        .cat_civ_ctrl_addr   = 0,
    },
    {
        .id                  = "xiegu-g90-digirig",
        .display_name        = "Xiegu G90+DigiRig",   // ≤20 chars for HOME row
        .description         = "Xiegu G90 HF via DigiRig; RTS PTT + CI-V CAT @19200",
        .ptt                 = NANOJS8_RADIO_PTT_RTS,
        // 300/200 ms matches MicroJS8's empirical numbers. Relay-based
        // PTT through DigiRig's optoisolator into G90.
        .ptt_on_delay_ms     = 300,
        .ptt_off_delay_ms    = 200,
        .cat                 = NANOJS8_RADIO_CAT_CIV,
        // G90 CAT baud (verified Radioddity official guide + cheat sheet)
        .cat_baud            = 19200,
        .can_set_freq        = true,
        // G90 default CI-V address. Some firmware revisions default to
        // 0x88 instead; if a user has changed it via the G90 menu, we
        // can add a SETUP row for cat_civ_radio_addr in a later layer.
        .cat_civ_radio_addr  = 0x70,
        // Generic controller address. Every CI-V radio knows to treat
        // 0xE0-0xEF as "host computer" and to ignore frames sent FROM
        // these addresses (i.e. the echoes of our own commands).
        .cat_civ_ctrl_addr   = 0xE0,
    },
};

constexpr size_t s_profile_count = sizeof(s_profiles) / sizeof(s_profiles[0]);

// Track whether we've already logged the "stored id unknown, falling
// back to default" warning, so we don't spam the log on every call to
// get_active() (which the status row calls every render).
std::atomic<bool> s_fallback_logged{false};

} // namespace

extern "C" size_t nanojs8_radio_count(void) {
    return s_profile_count;
}

extern "C" const nanojs8_radio_profile_t *nanojs8_radio_at(size_t index) {
    if (index >= s_profile_count) return nullptr;
    return &s_profiles[index];
}

extern "C" const nanojs8_radio_profile_t *nanojs8_radio_lookup(const char *id) {
    if (!id) return nullptr;
    for (size_t i = 0; i < s_profile_count; ++i) {
        if (strcmp(s_profiles[i].id, id) == 0) return &s_profiles[i];
    }
    return nullptr;
}

extern "C" int nanojs8_radio_index_of(const char *id) {
    if (!id) return -1;
    for (size_t i = 0; i < s_profile_count; ++i) {
        if (strcmp(s_profiles[i].id, id) == 0) return (int)i;
    }
    return -1;
}

extern "C" const nanojs8_radio_profile_t *nanojs8_radio_get_default(void) {
    return &s_profiles[0];
}

extern "C" const nanojs8_radio_profile_t *nanojs8_radio_get_active(void) {
    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (cfg && cfg->radio_id[0] != '\0') {
        const nanojs8_radio_profile_t *p = nanojs8_radio_lookup(cfg->radio_id);
        if (p) return p;
        // Stored id doesn't match a registered profile — could be a
        // stale id from before L6b.5 dropped trusdx-ts480 and ts480.
        if (!s_fallback_logged.exchange(true, std::memory_order_relaxed)) {
            ESP_LOGW(TAG, "Stored radio_id '%s' is not a registered profile; "
                          "using default '%s'",
                     cfg->radio_id, s_profiles[0].id);
        }
    }
    return &s_profiles[0];
}
