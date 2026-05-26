// NanoJS8 — Screen router.
//
// Owns the screen-ring state machine: which screen is active, how Tab
// cycles fields within it, and how LEFT/RIGHT navigate between screens
// in the ring. Each screen implements the IScreen interface; the router
// holds a registry and dispatches events.
//
// Phase 1: only the SETUP screen is registered. LEFT/RIGHT log a stub
// message ("screen ring not populated yet"). Phase 2 expands the ring
// to all 7 screens.

#pragma once

#include <cstdint>
#include "input_event.h"


namespace nanojs8 {
namespace ui {

// Stable identity for each screen in the ring. The numeric values are
// the ring order: LEFT moves toward SETUP, RIGHT moves toward DOCTOR.
// Phase 1 only implements SETUP; the others are reserved.
enum class ScreenId : uint8_t {
    SETUP    = 0,
    RECEIVE  = 1,
    COMPOSE  = 2,
    INBOX    = 3,
    GPS      = 4,
    MAP      = 5,
    DOCTOR   = 6,
    COUNT
};

// Screen interface. Each concrete screen provides a constexpr id() and
// implements handle_event() and render(). The router calls render()
// after every event dispatch and at a low background rate (~5 Hz) for
// time-based redraw needs (e.g. the clock in later screens).
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
};

// Router lifetime. Call once at startup.
void router_init();

// Per-tick service: poll the input translator, dispatch events,
// trigger render. Caller (the UI task) should invoke this at the UI
// tick rate (~20 Hz is comfortable).
void router_tick();

// Force an immediate redraw of the active screen. Used after
// configuration changes that the screen needs to reflect.
void router_redraw();

// Programmatically switch screens. Used by hotkeys or by screens
// themselves (e.g. SETUP's "saved" confirmation → RECEIVE in Phase 2).
void router_go_to(ScreenId target);

} // namespace ui
} // namespace nanojs8
