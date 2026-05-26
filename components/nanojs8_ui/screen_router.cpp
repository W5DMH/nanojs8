// NanoJS8 — Screen router implementation.

#include "screen_router.h"

#include <memory>

#include "esp_log.h"
#include <M5Cardputer.h>

#include "input_translator.h"
#include "screens/setup_screen.h"

namespace nanojs8 {
namespace ui {

static const char* TAG = "router";

// The active screen. unique_ptr so screens self-clean on swap.
//
// Phase 1 only ever holds a SetupScreen. Phase 2 will instantiate one
// screen per ScreenId and switch the pointer on LEFT/RIGHT events. For
// now we keep it simple — one screen, no switching machinery yet.
static std::unique_ptr<IScreen> s_active;

// Event buffer for one tick. The translator drains state.word which can
// hold multiple chars typed between polls; 8 is plenty of headroom for
// a 20 Hz tick rate with a human typing.
static constexpr size_t EVENT_BUF_SIZE = 8;

void router_init() {
    ESP_LOGI(TAG, "Router init — instantiating SETUP screen");
    s_active = std::unique_ptr<IScreen>(new screens::SetupScreen());
    s_active->on_enter();
    s_active->render();
}

void router_redraw() {
    if (s_active) {
        s_active->render();
    }
}

void router_go_to(ScreenId target) {
    // Phase 1 only has SETUP. Log and stay put for any other request.
    if (target == ScreenId::SETUP) {
        ESP_LOGD(TAG, "router_go_to: SETUP (no-op, already active)");
        return;
    }
    ESP_LOGW(TAG, "router_go_to: ScreenId %d not implemented in Phase 1",
             (int)target);
}

void router_tick() {
    if (!s_active) {
        return;
    }

    // Poll input.
    InputEvent events[EVENT_BUF_SIZE];
    const size_t n = poll(events, EVENT_BUF_SIZE);

    // Dispatch each event to the active screen. If the screen doesn't
    // consume it (handle_event returned false), the router considers it
    // for global handling.
    for (size_t i = 0; i < n; ++i) {
        const InputEvent& ev = events[i];

        // Log key events (not typed chars — that would be noisy when the
        // user is editing a field).
        if (ev.key != Key::NONE) {
            ESP_LOGD(TAG, "key %s", key_name(ev.key));
        }

        const bool consumed = s_active->handle_event(ev);
        if (consumed) {
            continue;
        }

        // Unconsumed. Global handlers:
        if (ev.key == Key::LEFT || ev.key == Key::RIGHT) {
            ESP_LOGI(TAG, "Screen-ring nav (%s) not populated yet in Phase 1",
                     key_name(ev.key));
        }
        // Other unconsumed events: silently ignore. We don't want stray
        // typed chars to log noise when a screen happens to be in a
        // mode that doesn't care.
    }

    // Repaint after dispatching (screens self-suppress redundant draws).
    s_active->render();
}

} // namespace ui
} // namespace nanojs8
