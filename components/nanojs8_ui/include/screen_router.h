// NanoJS8 — Screen router.
//
// Owns the screen-ring state machine: which screen is active, how Tab
// cycles fields within it, and how LEFT/RIGHT navigate between screens
// in the ring. Each screen implements the IScreen interface; the router
// holds a registry and dispatches events.
//
// Phase 2: all 7 screens registered. HOME displays current operator
// state (callsign, grid, GPS status etc.); SETUP is fully editable
// (Phase 1 behavior); the remaining 5 (HEARD, DIRECTED, INBOX,
// COMPOSE, ALLCALL) are placeholders that will be filled out in
// Phases 4-6.
//
// Screen ring order, matches MicroJS8 (minus the EMERGENCY screen
// which NanoJS8 replaces with the $GPS token in COMPOSE):
//   HOME → HEARD → DIRECTED → INBOX → COMPOSE → ALLCALL → SETUP → HOME

#pragma once

#include <cstdint>
#include "input_event.h"

namespace nanojs8 {
namespace ui {

// Stable identity for each screen in the ring. The numeric values
// reflect ring order (0 == HOME, RIGHT moves toward higher numbers
// up to SETUP, then wraps back to HOME).
enum class ScreenId : uint8_t {
    HOME      = 0,
    HEARD     = 1,
    DIRECTED  = 2,
    INBOX     = 3,
    COMPOSE   = 4,
    ALLCALL   = 5,
    SETUP     = 6,
    COUNT
};

// Screen interface. Each concrete screen provides a constexpr id() and
// implements handle_event() and render(). The router calls render()
// after every event dispatch and at a low background rate for time-
// based redraw needs (e.g. the clock in later screens).
class IScreen {
public:
    virtual ~IScreen() = default;
    virtual ScreenId id() const = 0;

    // Called once when the screen becomes active. Used to reset
    // transient state (cursor position in edit mode, etc.).
    virtual void on_enter() {}

    // Called once when the screen becomes inactive.
    virtual void on_leave() {}

    // Dispatch a single input event. Return true if the event was
    // consumed by the screen; false if the router should consider it
    // for global handling (e.g. LEFT/RIGHT for screen-ring nav).
    virtual bool handle_event(const InputEvent& ev) = 0;

    // Paint the screen. Called after every consumed event AND at a
    // background tick. Implementations should be idempotent and cheap
    // to call repeatedly; only redraw the regions that changed.
    virtual void render() = 0;

    // Is the screen currently in a text-editing mode? When true, the
    // router suppresses bare-arrow translation for `,` and `/` so the
    // operator can type those characters into a field. SETUP is the
    // only Phase 2 screen that uses this; others return false.
    //
    // Default returns false (screen is in nav mode, ring nav allowed).
    virtual bool is_editing_text() const { return false; }
};

// Router lifetime. Call once at startup, then call router_set_screen()
// before router_tick() begins running.
void router_init();

// Set the starting screen. Called by main() after determining whether
// the operator's callsign is configured (HOME) or not yet (SETUP).
// Must be called after router_init() and before the first router_tick().
void router_set_screen(ScreenId initial);

// Per-tick service: poll the input translator, dispatch events,
// trigger render. Caller (the UI task) should invoke this at the UI
// tick rate (~20 Hz is comfortable).
void router_tick();

// Force an immediate redraw of the active screen. Used after
// configuration changes that the screen needs to reflect.
void router_redraw();

// Programmatically switch screens. Used by hotkeys or by screens
// themselves (e.g. SETUP commit → HOME in Phase 5+).
void router_go_to(ScreenId target);

} // namespace ui
} // namespace nanojs8
