/*
 * ui.cpp — NanoJS8 v0.7 UI dispatcher (Layer 6b.2)
 * ====================================================
 * Routes render/input calls to the currently-active screen. Owns the
 * "current screen" pointer and the "needs full redraw" flag.
 *
 * Threading: all functions are intended for the main loop thread.
 * Not safe to call from other tasks without external locking.
 *
 * License: GPL-3.0
 */

#include "ui.h"
#include "ui_internal.h"
#include "config.h"

#include "esp_log.h"

static const char* TAG = "ui";

// Screen registry. Each entry's id field MUST match its index here so
// nanojs8_ui_set_screen(NANOJS8_SCREEN_X) finds the right one in O(1).
static const nanojs8_screen_t* const s_screens[NANOJS8_SCREEN_COUNT] = {
    &SCREEN_HOME,
    &SCREEN_SETUP,
    &SCREEN_HEARD,         // L7.9:    index 2
    &SCREEN_ALL,           // L7.10:   index 3 — full activity log
    &SCREEN_DIRECTED,      // L7.10:   index 4 — filtered to me/groups
    &SCREEN_INBOX,         // L7.11g.3: index 5 — mailbox list
    &SCREEN_INBOX_DETAIL,  // L7.11g.3: index 6 — modal detail (off-ring)
    &SCREEN_COMPOSE,       // L7.11f:  index 7 — multi-field TX form
    &SCREEN_ALLCALL,       // L7.11e:  index 8 — TX menu (HEARTBEAT/QUERY MSGS/CQ)
};

static nanojs8_screen_id_t s_current_screen = NANOJS8_SCREEN_HOME;
static bool                s_needs_full_redraw = true;
static uint8_t             s_last_input = 0;
static bool                s_initialized = false;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_ui_init(void) {
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "UI subsystem starting");

    // Pick the initial screen. Mirroring MicroJS8: if the station
    // isn't configured (callsign or grid unset), force the operator
    // through Setup before showing Home.
    if (nanojs8_config_is_configured()) {
        s_current_screen = NANOJS8_SCREEN_HOME;
        ESP_LOGI(TAG, "  Station configured → initial screen = HOME");
    } else {
        s_current_screen = NANOJS8_SCREEN_SETUP;
        ESP_LOGI(TAG, "  Station NOT configured → initial screen = SETUP");
    }

    // Defensive: confirm the screen registry matches the enum order.
    // A mismatch here would silently send events to the wrong screen.
    for (int i = 0; i < NANOJS8_SCREEN_COUNT; ++i) {
        if (s_screens[i] == nullptr ||
            s_screens[i]->id != (nanojs8_screen_id_t)i) {
            ESP_LOGE(TAG, "  Screen registry mismatch at index %d", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Paint the initial screen.
    const nanojs8_screen_t* screen = s_screens[s_current_screen];
    if (screen->on_enter) {
        screen->on_enter();
    }
    s_needs_full_redraw = true;
    s_initialized = true;

    ESP_LOGI(TAG, "UI ready (current=%s)", screen->name);
    return ESP_OK;
}

extern "C" void nanojs8_ui_render(void) {
    if (!s_initialized) return;
    const nanojs8_screen_t* screen = s_screens[s_current_screen];
    if (screen && screen->render) {
        screen->render(s_needs_full_redraw);
        s_needs_full_redraw = false;
    }
}

extern "C" void nanojs8_ui_handle_input(uint8_t event) {
    if (!s_initialized) return;
    if (event == 0) return;     // sentinel for "no event"

    s_last_input = event;

    const nanojs8_screen_t* screen = s_screens[s_current_screen];
    if (screen && screen->handle_input) {
        (void)screen->handle_input(event);
        // Screen may have called set_screen() during input — that's
        // fine, our next render() will pick up the new screen and
        // its on_enter has already painted the chrome.
    }
}

extern "C" void nanojs8_ui_set_screen(nanojs8_screen_id_t id) {
    if (!s_initialized) return;
    if (id == s_current_screen) return;
    if ((int)id < 0 || (int)id >= NANOJS8_SCREEN_COUNT) {
        ESP_LOGW(TAG, "  set_screen ignored: invalid id %d", (int)id);
        return;
    }

    const nanojs8_screen_t* from = s_screens[s_current_screen];
    const nanojs8_screen_t* to   = s_screens[id];
    ESP_LOGI(TAG, "Screen switch: %s → %s", from->name, to->name);

    s_current_screen = id;
    s_needs_full_redraw = true;

    if (to->on_enter) {
        to->on_enter();
    }
}

extern "C" nanojs8_screen_id_t nanojs8_ui_current_screen(void) {
    return s_current_screen;
}

extern "C" uint8_t nanojs8_ui_last_input(void) {
    return s_last_input;
}
