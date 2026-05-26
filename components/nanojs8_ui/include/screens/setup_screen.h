// NanoJS8 — SETUP screen.
//
// The first screen of the 7-screen ring. Lets the operator enter
// callsign, grid, and radio profile. Persists to NVS on Ctrl+S.
//
// Layout (240×135 landscape):
//
//   +----------------------------------------+
//   | SETUP                              v0.1|  ← header
//   |                                        |
//   |   CALL :  W5DMH                        |  ← field rows
//   |   GRID :  EM10                         |
//   |   RADIO:  qdx                          |
//   |                                        |
//   |                                        |
//   | Tab next · Enter edit · ^S save        |  ← footer hint
//   +----------------------------------------+
//
// One field is highlighted as "focused" at any time. Tab moves focus
// forward through CALL → GRID → RADIO → CALL (wraps).
//
// State machine within the screen:
//   NAV mode:  focused field shown with a marker. Tab/UP/DOWN navigate.
//              Enter → enter EDIT mode on focused field.
//   EDIT mode: cursor shown inside the field value. Typed chars append,
//              Backspace deletes, Enter commits (after validation),
//              ESC cancels (reverts to last committed value).
//              UP/DOWN cycle options on menu fields (RADIO).
//
// Validation policy (Q3 = Option B):
//   Enter while editing a text field with an invalid value REFUSES to
//   commit and shows a brief red error banner. User must fix the value
//   or press ESC to cancel and revert.

#pragma once

#include "screen_router.h"

namespace nanojs8 {
namespace ui {
namespace screens {

class SetupScreen : public IScreen {
public:
    SetupScreen();

    ScreenId id() const override { return ScreenId::SETUP; }
    void on_enter() override;
    bool handle_event(const InputEvent& ev) override;
    void render() override;

    // Tells the router that bare `,` `/` `;` `.` should be typed chars
    // (NOT screen-ring nav) when we're in EDIT on a text field. Returns
    // false in NAV mode and in menu-field EDIT, so ring nav still works
    // from the SETUP screen when not actively typing.
    bool is_editing_text() const override;

public:
    // Field identity. Order here is Tab cycle order on the screen.
    enum class Field : uint8_t {
        CALL   = 0,
        GRID   = 1,
        RADIO  = 2,
        GROUPS = 3,
        COUNT
    };

    // Mode within the screen.
    enum class Mode : uint8_t {
        NAV  = 0,    // navigation: Tab/UP/DOWN moves focus
        EDIT = 1,    // editing focused field
    };

    // State.
    Field    focus_;         // currently focused field
    Mode     mode_;          // NAV or EDIT
    bool     dirty_;         // true if in-memory config differs from on-disk
    char     edit_buf_[64];  // draft value during EDIT (sized for GROUPS, the longest field)
    uint8_t  edit_len_;      // current length of edit_buf_ (excl. NUL)
    bool     show_error_;    // true → render the error banner once
    bool     show_saved_;    // true → render the "Saved" banner once
    uint32_t banner_clear_at_ms_;  // millis() after which to clear banners

    // Render-cache state. We only repaint when something changed.
    bool needs_full_redraw_;

    // Helpers.
    void enter_edit_mode();
    void commit_edit_mode();      // tries to commit; sets show_error_ on fail
    void cancel_edit_mode();      // reverts edit_buf_, returns to NAV
    void cycle_focus_forward();   // Tab handler
    void cycle_radio_option(bool forward);  // UP/DOWN in RADIO field
    bool field_is_menu(Field f) const;
    bool save_all();              // Ctrl+S handler. Returns success.

    // Rendering helpers.
    void draw_full();             // full repaint
    void draw_banner();           // overlay banner if show_error_ or show_saved_
};

} // namespace screens
} // namespace ui
} // namespace nanojs8
