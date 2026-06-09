/*
 * ui.h — NanoJS8 v0.7 multi-screen UI framework (Layer 6b.2)
 * ============================================================
 * Provides a screen-stack abstraction for the T-Deck UI. Each screen
 * is implemented as a separate module (screen_home.cpp, screen_setup.cpp)
 * registered with the dispatcher in ui.cpp.
 *
 * Main loop integration:
 *   1. Call nanojs8_ui_init() once at boot, AFTER config/display.
 *   2. In the main loop, drain input events and pass each to
 *      nanojs8_ui_handle_input(event).
 *   3. Call nanojs8_ui_render() each loop iteration to refresh the
 *      current screen's dynamic content.
 *
 * Screen switching:
 *   At boot, the current screen is selected automatically:
 *     - SETUP if nanojs8_config_is_configured() returns false
 *       (first boot or after a factory reset)
 *     - HOME otherwise
 *   At runtime, screens may request a switch via nanojs8_ui_set_screen()
 *   from their handle_input callback (e.g. trackball-right on HOME to
 *   open SETUP, or trackball-left on a fully-configured SETUP to return
 *   to HOME).
 *
 * Layer 6b.2 scope:
 *   - HOME screen: header + status rows (keyboard, audio, serial,
 *     trackball, config). Replaces the inline boot banner + status bar
 *     that lived in main.c through L6b.1.
 *   - SETUP screen: 6-row layout matching MicroJS8 (Call, Grid, Groups,
 *     Units, Freq, Radio) with focus navigation via trackball UP/DOWN.
 *     READ-ONLY in this layer; edit mode lands in L6b.3.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NANOJS8_SCREEN_HOME         = 0,
    NANOJS8_SCREEN_SETUP        = 1,
    NANOJS8_SCREEN_HEARD        = 2,    // L7.9
    NANOJS8_SCREEN_ALL          = 3,    // L7.10: full activity log (no filter)
    NANOJS8_SCREEN_DIRECTED     = 4,    // L7.9, filtered in L7.10
    NANOJS8_SCREEN_INBOX        = 5,    // L7.11g.3: mailbox list (UNREAD/READ/STORE/DELIVERED)
    NANOJS8_SCREEN_INBOX_DETAIL = 6,    // L7.11g.3: modal detail view (entered via ENTER on INBOX)
    NANOJS8_SCREEN_COMPOSE      = 7,    // L7.11f: multi-field TX form (10 verbs)
    NANOJS8_SCREEN_ALLCALL      = 8,    // L7.11e: 3-row TX menu (HEARTBEAT/QUERY MSGS/CQ)
    NANOJS8_SCREEN_COUNT
} nanojs8_screen_id_t;

// Initialize the UI subsystem. Picks the initial screen based on
// nanojs8_config_is_configured() and calls the screen's on_enter
// to paint the first frame.
//
// REQUIRES: nanojs8_display_init() and nanojs8_config_init() already
// successful. Returns ESP_OK on success; logs and returns the error
// from a screen's on_enter callback if anything goes wrong.
esp_err_t nanojs8_ui_init(void);

// Render the current screen. Internally tracks whether a full repaint
// is needed (set by set_screen) vs an incremental update (the default
// for status bar etc.). Safe to call every main-loop iteration.
void nanojs8_ui_render(void);

// Dispatch an input event to the current screen. `event` is one of:
//   - Keyboard ASCII (0x20..0x7E for printable, plus 0x08 BS, 0x0D CR,
//     0x09 TAB, 0x1B ESC from the C3 keyboard)
//   - Trackball virtual key (0x82..0x86 — UP/DOWN/LEFT/RIGHT/CLICK)
//   - 0 means "no event" and is ignored
//
// The screen's handle_input callback may choose to consume the event
// (move focus, etc.) or to ignore it. Screen switches happen via
// set_screen() called from inside handle_input.
void nanojs8_ui_handle_input(uint8_t event);

// Switch to a different screen. Sets the redraw flag and calls the
// destination screen's on_enter so it can paint its static elements.
// No-op if the target is the current screen.
void nanojs8_ui_set_screen(nanojs8_screen_id_t id);

// Query the currently-active screen.
nanojs8_screen_id_t nanojs8_ui_current_screen(void);

// The most recent input event seen by handle_input. HOME displays
// this in its keyboard row (replacing main.c's local last_displayed_key).
// Returns 0 if no input has been seen since boot.
uint8_t nanojs8_ui_last_input(void);

#ifdef __cplusplus
}
#endif
