// NanoJS8 — Screen router implementation (Phase 2).
//
// Owns one instance of each ScreenId, holds them in a fixed array, and
// dispatches events to the active one. Bare `,` `/` chars become
// LEFT/RIGHT ring-nav events UNLESS the active screen reports it is
// editing a text field, in which case the chars pass through to the
// screen as typed input. Same translation applies to bare `;` `.`
// (UP/DOWN) but those are reserved for screen-internal scrolling in
// later phases.

#include "screen_router.h"

#include <array>
#include <memory>

#include "esp_log.h"
#include <M5Cardputer.h>

#include "input_translator.h"
#include "power_manager.h"
#include "screens/home_screen.h"
#include "screens/placeholder_screen.h"
#include "screens/setup_screen.h"

namespace nanojs8 {
namespace ui {

static const char* TAG = "router";

// Registry: one unique_ptr per ScreenId, indexed by enum value.
// Sized to ScreenId::COUNT so iterating is straightforward.
static std::array<std::unique_ptr<IScreen>, (size_t)ScreenId::COUNT> s_screens;

// Currently-active screen identity. Defaults to HOME; main() calls
// router_set_screen() before the first tick to choose the actual
// starting screen based on operator config state.
static ScreenId s_active_id = ScreenId::HOME;

// Event buffer for one tick. The translator drains state.word which can
// hold multiple chars typed between polls; 8 is plenty of headroom for
// a 20 Hz tick rate with a human typing.
static constexpr size_t EVENT_BUF_SIZE = 8;

// Helper to get the active screen pointer (or nullptr if the registry
// hasn't been populated yet).
static IScreen* active_screen() {
    const size_t idx = (size_t)s_active_id;
    if (idx >= s_screens.size()) {
        return nullptr;
    }
    return s_screens[idx].get();
}

// Translate a bare-char nav event into the corresponding key when
// appropriate. Returns the translated event, or the original event
// unchanged if no translation applies.
//
// Translation rules:
//   - Only typed chars (ev.ch != 0) are candidates.
//   - The active screen must NOT be editing a text field.
//   - `,` → LEFT (prev screen), `/` → RIGHT (next screen)
//   - `;` → UP, `.` → DOWN (reserved for screen-internal scrolling
//     in Phases 4+; in Phase 2 they're dispatched as UP/DOWN events
//     which screens currently ignore).
static InputEvent translate_bare_arrow(const InputEvent& ev) {
    if (ev.ch == 0) {
        return ev;
    }
    IScreen* screen = active_screen();
    if (screen && screen->is_editing_text()) {
        return ev;  // pass through as typed char
    }
    switch (ev.ch) {
        case ',': return ev_key(Key::LEFT);
        case '/': return ev_key(Key::RIGHT);
        case ';': return ev_key(Key::UP);
        case '.': return ev_key(Key::DOWN);
        default:  return ev;
    }
}

// Switch to a new screen, calling on_leave/on_enter and forcing a
// repaint. Safe to call with the current screen as target (no-op).
static void switch_to(ScreenId target) {
    if (target == s_active_id) {
        return;
    }
    const size_t idx = (size_t)target;
    if (idx >= s_screens.size() || !s_screens[idx]) {
        ESP_LOGW(TAG, "switch_to: ScreenId %d not registered, staying put",
                 (int)target);
        return;
    }
    IScreen* old_screen = active_screen();
    if (old_screen) {
        old_screen->on_leave();
    }
    s_active_id = target;
    IScreen* new_screen = active_screen();
    new_screen->on_enter();
    new_screen->render();
    // Reset the input translator so any modifier keys held during the
    // switch don't trigger stale events on the new screen.
    reset();
}

// Cycle the active screen forward (RIGHT) or backward (LEFT). Wraps
// at both ends (matches MicroJS8).
static void cycle_screen(bool forward) {
    const size_t count = (size_t)ScreenId::COUNT;
    size_t idx = (size_t)s_active_id;
    if (forward) {
        idx = (idx + 1) % count;
    } else {
        idx = (idx == 0) ? (count - 1) : (idx - 1);
    }
    ScreenId target = (ScreenId)idx;
    ESP_LOGI(TAG, "Ring nav: %s → ScreenId %d",
             forward ? "RIGHT" : "LEFT", (int)target);
    switch_to(target);
}

void router_init() {
    ESP_LOGI(TAG, "Router init — instantiating 7 screens");

    // HOME — real content (read from NVS).
    s_screens[(size_t)ScreenId::HOME] =
        std::unique_ptr<IScreen>(new screens::HomeScreen());

    // Five placeholders for the screens that get real implementations
    // in Phases 4-6. Each is given its ScreenId, header text, and a
    // short note explaining when it'll be filled in.
    s_screens[(size_t)ScreenId::HEARD] = std::unique_ptr<IScreen>(
        new screens::PlaceholderScreen(
            ScreenId::HEARD, "HEARD",
            "Phase 4 - audio decode pending"));

    s_screens[(size_t)ScreenId::DIRECTED] = std::unique_ptr<IScreen>(
        new screens::PlaceholderScreen(
            ScreenId::DIRECTED, "DIRECTED",
            "Phase 4 - decode activity log pending"));

    s_screens[(size_t)ScreenId::INBOX] = std::unique_ptr<IScreen>(
        new screens::PlaceholderScreen(
            ScreenId::INBOX, "INBOX",
            "Phase 6 - mailbox pending"));

    s_screens[(size_t)ScreenId::COMPOSE] = std::unique_ptr<IScreen>(
        new screens::PlaceholderScreen(
            ScreenId::COMPOSE, "COMPOSE",
            "Phase 5 - TX compose pending"));

    s_screens[(size_t)ScreenId::ALLCALL] = std::unique_ptr<IScreen>(
        new screens::PlaceholderScreen(
            ScreenId::ALLCALL, "ALLCALL",
            "Phase 5 - heartbeat broadcast pending"));

    // SETUP — full Phase 1 implementation.
    s_screens[(size_t)ScreenId::SETUP] =
        std::unique_ptr<IScreen>(new screens::SetupScreen());

    // Don't activate any screen yet — main() picks HOME or SETUP via
    // router_set_screen() based on whether callsign is configured.
}

void router_set_screen(ScreenId initial) {
    const size_t idx = (size_t)initial;
    if (idx >= s_screens.size() || !s_screens[idx]) {
        ESP_LOGE(TAG, "router_set_screen: ScreenId %d invalid, defaulting to HOME",
                 (int)initial);
        s_active_id = ScreenId::HOME;
    } else {
        s_active_id = initial;
    }
    IScreen* screen = active_screen();
    if (screen) {
        screen->on_enter();
        screen->render();
        ESP_LOGI(TAG, "Active screen: ScreenId %d", (int)s_active_id);
    }
}

void router_redraw() {
    IScreen* screen = active_screen();
    if (screen) {
        screen->render();
    }
}

void router_go_to(ScreenId target) {
    switch_to(target);
}

void router_tick() {
    IScreen* screen = active_screen();
    if (!screen) {
        return;
    }

    // Poll input.
    InputEvent events[EVENT_BUF_SIZE];
    const size_t n = poll(events, EVENT_BUF_SIZE);

    // Dispatch each event. Bare-arrow chars are translated to
    // LEFT/RIGHT/UP/DOWN keys when the screen isn't editing text.
    for (size_t i = 0; i < n; ++i) {
        InputEvent ev = translate_bare_arrow(events[i]);

        // ── Power management hooks (Phase 3.5) ──────────────────────────
        // Any input is "activity": resets the idle timer. If the screen
        // was dimmed/blanked or we were in charge mode, this keypress is
        // consumed purely to WAKE — it does not also act on the active
        // screen (so mashing a key to wake doesn't accidentally trigger
        // a screen action). notify_activity() performs the wake.
        const bool was_suppressed = nanojs8::power::ui_rendering_suppressed()
                                     || nanojs8::power::in_charge_mode();
        nanojs8::power::notify_activity();
        if (was_suppressed) {
            // Screen was off/charging; this key only wakes it. Eat it.
            continue;
        }

        // Ctrl+C enters charge mode (production entry; serial `charge`
        // is the dev entry). Handle before screen dispatch so no screen
        // can swallow it.
        if (ev.key == Key::CTRL_C) {
            ESP_LOGI(TAG, "Ctrl+C → entering charge mode");
            nanojs8::power::enter_charge_mode();
            continue;
        }

        // Log key events at DEBUG (typed chars stay quiet to avoid
        // noise during text editing).
        if (ev.key != Key::NONE) {
            ESP_LOGD(TAG, "key %s", key_name(ev.key));
        }

        const bool consumed = screen->handle_event(ev);
        if (consumed) {
            continue;
        }

        // Screen didn't consume — global handling:
        if (ev.key == Key::LEFT) {
            cycle_screen(false);
        } else if (ev.key == Key::RIGHT) {
            cycle_screen(true);
        } else if (ev.key == Key::UP || ev.key == Key::DOWN) {
            // No global UP/DOWN handler in Phase 2. Screens that care
            // (HEARD/DIRECTED/INBOX in Phase 4+) handle these for row
            // scrolling. Silent drop here.
        }
        // Other unconsumed events: silent.
    }

    // Apply any pending power-driven screen change (dim/blank/charge/wake)
    // here in the UI task, which owns the display bus. The power monitor
    // task only records the desired state; this is where M5GFX is touched.
    nanojs8::power::apply_pending_screen_change();

    // Repaint after dispatching, UNLESS the screen is blanked or in
    // charge mode — no point rendering to a dark panel, and in charge
    // mode the panel is asleep so drawing would be wasted work (and
    // could wake the panel controller). Screens self-suppress redundant
    // draws; this is the additional power-state suppression.
    if (nanojs8::power::ui_rendering_suppressed()) {
        return;
    }
    screen = active_screen();  // refresh; switch_to may have changed it
    if (screen) {
        screen->render();
    }
}

} // namespace ui
} // namespace nanojs8
