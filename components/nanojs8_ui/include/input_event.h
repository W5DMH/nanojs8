// NanoJS8 — Logical input events.
//
// Mirrors the event taxonomy in MicroJS8's src/microjs8/input/events.py
// so screen code can be ported between the two projects with minimal
// translation. The wire-protocol mapping (M5Cardputer keysState → these
// events) lives in input_translator.cpp.
//
// Why an enum + optional char rather than raw keycodes: lets screens
// dispatch on logical intent ("user wants to cancel") rather than
// hardware quirk ("Fn+backtick was held"). The dispatch in screen code
// is a switch on Key with a single `char` extracted for printable input.

#pragma once

#include <cstdint>

namespace nanojs8 {
namespace ui {

// Logical key set. Order matches MicroJS8's Key enum where possible.
// LEFT/RIGHT are reserved for screen-ring cycling (Phase 2 wires them up);
// UP/DOWN are for menu-field option cycling within the focused field.
enum class Key : uint8_t {
    NONE = 0,        // No key — sentinel value, never dispatched.
    LEFT,            // Previous screen in the ring (Phase 2+)
    RIGHT,           // Next screen in the ring (Phase 2+)
    UP,              // Cycle menu options up (within focused menu field)
    DOWN,            // Cycle menu options down
    ENTER,           // Commit field edit / activate selection
    ESC,             // Cancel current edit, revert to last committed
    TAB,             // Next field within current screen (wraps)
    BACKSPACE,       // Delete previous char while editing text
    SPACE,           // Space char while editing (separate so handlers can ignore in nav mode)
    DELETE,          // Forward delete (unused in Phase 1, reserved)
    CTRL_S,          // Save all to NVS
    CTRL_Q,          // Quit (unused on embedded; reserved for parity with MicroJS8)
    CTRL_H,          // Help (unused in Phase 1; reserved)
    CTRL_C,          // Enter charge mode (Phase 3.5 power management)
};

// One input event. Exactly one of `key != NONE` or `ch != 0` is true.
// Field text input arrives as InputEvent{ch='a'}; nav/action arrives as
// InputEvent{key=Key::TAB}. Screens dispatch on whichever is set.
struct InputEvent {
    Key  key = Key::NONE;
    char ch  = 0;
};

// Convenience constructors. Used by input_translator and tests.
inline InputEvent ev_key(Key k)  { InputEvent e; e.key = k; return e; }
inline InputEvent ev_char(char c){ InputEvent e; e.ch  = c; return e; }

// Human-readable name for logging (DOCTOR screen prints recent keys).
const char* key_name(Key k);

} // namespace ui
} // namespace nanojs8
