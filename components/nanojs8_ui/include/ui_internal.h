/*
 * ui_internal.h — NanoJS8 v0.7 UI internals (Layer 6b.2)
 * ========================================================
 * Shared declarations between the UI dispatcher (ui.cpp) and the
 * individual screen modules (screen_home.cpp, screen_setup.cpp).
 * Not part of the public API — never include this from outside the
 * nanojs8_ui component.
 */
#pragma once

#include "ui.h"

// Screen callback signatures.
//
// render(full_redraw):
//   Called from nanojs8_ui_render() once per main-loop iteration.
//   When full_redraw is true, the screen should repaint all of its
//   contents (e.g. on entry, or after a screen switch). When false,
//   only dynamic regions need updating — saves SPI bandwidth.
//
// handle_input(event):
//   Called from nanojs8_ui_handle_input() when an event arrives.
//   Returns true if the event was consumed. Reserved for future
//   parent-screen propagation; ignored at the moment.
//
// on_enter():
//   Called once when the screen becomes the current screen — by
//   nanojs8_ui_init() at boot or by nanojs8_ui_set_screen() at runtime.
//   This is where the screen clears the display and paints static
//   chrome (header, separator lines, footer hints). render() does
//   not need to redraw what on_enter already painted.
typedef void (*screen_render_fn)(bool full_redraw);
typedef bool (*screen_input_fn)(uint8_t event);
typedef void (*screen_enter_fn)(void);

typedef struct nanojs8_screen_s {
    nanojs8_screen_id_t id;
    const char *name;          // For logs
    screen_render_fn render;
    screen_input_fn handle_input;
    screen_enter_fn on_enter;
} nanojs8_screen_t;

// Each screen module exports one of these.
extern const nanojs8_screen_t SCREEN_HOME;
extern const nanojs8_screen_t SCREEN_SETUP;
extern const nanojs8_screen_t SCREEN_HEARD;         // L7.9: screen_heard.cpp
extern const nanojs8_screen_t SCREEN_ALL;           // L7.10: screen_all.cpp
extern const nanojs8_screen_t SCREEN_DIRECTED;      // L7.9, filtered in L7.10
extern const nanojs8_screen_t SCREEN_INBOX;         // L7.11g.3: mailbox list
extern const nanojs8_screen_t SCREEN_INBOX_DETAIL;  // L7.11g.3: modal detail view
extern const nanojs8_screen_t SCREEN_COMPOSE;       // L7.11f: multi-field TX form
extern const nanojs8_screen_t SCREEN_ALLCALL;       // L7.11e: TX menu (HB/QUERY/CQ)
