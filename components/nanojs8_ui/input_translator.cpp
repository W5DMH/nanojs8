// NanoJS8 — Input translator implementation.
//
// Reads M5Cardputer.Keyboard.keysState() each call and produces edge-
// detected logical events. The Cardputer ADV has no physical arrow or
// Esc keys, so we synthesize those via Fn-modifier combinations to
// match MicroJS8's logical key set:
//
//   Fn + ;   →  UP
//   Fn + .   →  DOWN
//   Fn + ,   →  LEFT       (previous screen)
//   Fn + /   →  RIGHT      (next screen)
//   Fn + `   →  ESC        (no physical Esc on the Cardputer)
//
// Tab, Enter, Backspace, Space are dedicated physical keys, surfaced
// directly by the keysState struct as boolean flags.
//
// Ctrl combos (Ctrl+S, Ctrl+Q, Ctrl+H) consume the modifier so the
// char doesn't also land in the text-input stream.

#include "input_translator.h"

#include <cstring>

#include "esp_log.h"
#include <M5Cardputer.h>

namespace nanojs8 {
namespace ui {

static const char* TAG = "input";

// Previous-frame state used for press-edge detection. Anything that was
// pressed last frame AND still pressed this frame is held (suppressed).
// Anything pressed this frame but NOT last frame is a fresh press → emit.
//
// We track only the boolean flags; the text-input char stream
// (state.word) is naturally edge-driven by M5Cardputer's own
// updateKeysState() — it only appends a char on the press-edge, and
// our consumer-side clear() empties the queue each poll. So we don't
// need to track previous chars ourselves.
struct PrevState {
    bool tab   = false;
    bool enter = false;
    bool del   = false;
    bool space = false;
    // Fn+char combos. We track the previous frame's Fn-held-AND-char
    // state per relevant char so the user holding Fn doesn't auto-
    // repeat UP/DOWN/LEFT/RIGHT/ESC.
    bool fn_semicolon = false;  // Fn+;  → UP
    bool fn_period    = false;  // Fn+.  → DOWN
    bool fn_comma     = false;  // Fn+,  → LEFT
    bool fn_slash     = false;  // Fn+/  → RIGHT
    bool fn_backtick  = false;  // Fn+`  → ESC
};

static PrevState s_prev = {};
static bool      s_first_poll = true;

void reset() {
    s_prev = PrevState{};
    s_first_poll = true;
}

// Helper to push an event into the caller's buffer with overflow guard.
static bool push(InputEvent* out, size_t out_cap, size_t& count, InputEvent ev) {
    if (count >= out_cap) {
        ESP_LOGD(TAG, "event buffer full, dropping event");
        return false;
    }
    out[count++] = ev;
    return true;
}

// Helper: was the char `c` typed this frame? Scans state.word and
// returns the FIRST matching position so the caller can erase it
// (preventing the same char from also landing in the text stream
// when it's part of a Fn combo).
//
// Returns SIZE_MAX if not found.
static size_t find_char_in_word(const std::vector<char>& word, char c) {
    for (size_t i = 0; i < word.size(); ++i) {
        if (word[i] == c) {
            return i;
        }
    }
    return SIZE_MAX;
}

size_t poll(InputEvent* out, size_t out_cap) {
    if (!out || out_cap == 0) {
        return 0;
    }

    M5Cardputer.update();
    M5Cardputer.Keyboard.updateKeysState();
    auto& state = M5Cardputer.Keyboard.keysState();

    size_t count = 0;

    // First-poll dampening: if a key happens to be held at boot, we don't
    // want to inject a press-edge event from the very first sample. Just
    // snapshot current state into "prev" and skip emission for this call.
    if (s_first_poll) {
        s_prev.tab   = state.tab;
        s_prev.enter = state.enter;
        s_prev.del   = state.del;
        s_prev.space = state.space;
        // For Fn combos at first-poll, examining the char stream once is
        // sufficient: snapshot Fn-held-with-each-target-char as "was held".
        if (state.fn) {
            s_prev.fn_semicolon = (find_char_in_word(state.word, ';') != SIZE_MAX);
            s_prev.fn_period    = (find_char_in_word(state.word, '.') != SIZE_MAX);
            s_prev.fn_comma     = (find_char_in_word(state.word, ',') != SIZE_MAX);
            s_prev.fn_slash     = (find_char_in_word(state.word, '/') != SIZE_MAX);
            s_prev.fn_backtick  = (find_char_in_word(state.word, '`') != SIZE_MAX);
        }
        state.word.clear();  // drop any chars held at boot
        s_first_poll = false;
        return 0;
    }

    // ─── Fn-combo navigation keys ────────────────────────────────────
    // Check Fn-combos FIRST and remove the char from state.word so it
    // doesn't also propagate as a typed char.
    //
    // We use the structure: "this frame the Fn key is held AND the
    // target char appears in this frame's word vector." Edge-detected
    // against the previous frame's combined state.
    if (state.fn) {
        struct FnCombo {
            char  ch;       // the combo char
            Key   key;      // logical key it maps to
            bool* prev;     // pointer to previous-frame flag
        };
        FnCombo combos[] = {
            { ';', Key::UP,    &s_prev.fn_semicolon },
            { '.', Key::DOWN,  &s_prev.fn_period    },
            { ',', Key::LEFT,  &s_prev.fn_comma     },
            { '/', Key::RIGHT, &s_prev.fn_slash     },
            { '`', Key::ESC,   &s_prev.fn_backtick  },
        };
        for (auto& c : combos) {
            const size_t pos = find_char_in_word(state.word, c.ch);
            const bool   pressed_now = (pos != SIZE_MAX);
            if (pressed_now && !*c.prev) {
                push(out, out_cap, count, ev_key(c.key));
            }
            *c.prev = pressed_now;
            if (pos != SIZE_MAX) {
                // Remove this char so it doesn't also get typed.
                state.word.erase(state.word.begin() + pos);
            }
        }
    } else {
        // Fn released — reset all Fn-combo prev flags so re-pressing
        // Fn+key after release emits a fresh event.
        s_prev.fn_semicolon = false;
        s_prev.fn_period    = false;
        s_prev.fn_comma     = false;
        s_prev.fn_slash     = false;
        s_prev.fn_backtick  = false;
    }

    // ─── Dedicated physical keys (edge-detected) ─────────────────────
    if (state.tab && !s_prev.tab) {
        push(out, out_cap, count, ev_key(Key::TAB));
    }
    s_prev.tab = state.tab;

    if (state.enter && !s_prev.enter) {
        push(out, out_cap, count, ev_key(Key::ENTER));
    }
    s_prev.enter = state.enter;

    if (state.del && !s_prev.del) {
        push(out, out_cap, count, ev_key(Key::BACKSPACE));
    }
    s_prev.del = state.del;

    // Space is treated specially: in nav mode, screens may want to
    // ignore it; in edit mode, it's a regular printable char. We emit
    // both an ev_key(SPACE) (for nav handlers) AND a typed ' ' (for
    // text handlers). The screen decides which to consume.
    // To avoid double-handling, screens should consume ev_key(SPACE)
    // only when in nav mode and let the typed ' ' through in edit mode.
    //
    // Actually simpler: emit only the typed ' ' (since M5Cardputer
    // already puts space in state.word). The Key::SPACE enum stays in
    // the taxonomy for future use but is currently unemitted.
    s_prev.space = state.space;

    // ─── Ctrl combos (Ctrl+S, Ctrl+Q, Ctrl+H) ────────────────────────
    // M5Cardputer reports modifier-held via state.ctrl. When ctrl is
    // held AND a target char appears in state.word, we emit the combo
    // event and consume the char to keep it out of the text stream.
    if (state.ctrl) {
        struct CtrlCombo {
            char ch;
            Key  key;
        };
        // 'S', 'Q', 'H' are upper-cased because M5Cardputer's
        // Cardputer ADV keyboard reports ctrl-held chars as uppercase.
        // We also accept lowercase to be safe across keyboard variants.
        CtrlCombo combos[] = {
            { 'S', Key::CTRL_S }, { 's', Key::CTRL_S },
            { 'Q', Key::CTRL_Q }, { 'q', Key::CTRL_Q },
            { 'H', Key::CTRL_H }, { 'h', Key::CTRL_H },
        };
        for (auto& c : combos) {
            const size_t pos = find_char_in_word(state.word, c.ch);
            if (pos != SIZE_MAX) {
                push(out, out_cap, count, ev_key(c.key));
                state.word.erase(state.word.begin() + pos);
            }
        }
    }

// ─── Remaining typed printable chars (edge-detected) ─────────────
    // M5Cardputer's updateKeysState() does NOT edge-detect chars — it
    // appends every currently-pressed key to state.word every poll. At
    // our 20 Hz tick rate, a 100 ms keypress would produce 2 chars
    // without suppression. We compare each frame's word against the
    // previous frame's and emit only chars that newly appeared.
    //
    // Limitation: typing the same char twice within 50 ms is suppressed.
    // This is acceptable — human typing is rarely faster than 150 ms
    // per keystroke, and the M5Cardputer matrix's own scan rate is
    // slower than that anyway.
    static std::vector<char> prev_word;
    for (char c : state.word) {
        if (c < 0x20 || c >= 0x7F) {
            continue;  // printable ASCII only
        }
        // Suppress if this char was also present in the previous frame
        // (key is being held). The first frame where the char appears
        // is the press-edge; subsequent frames are "held" — suppress.
        bool was_held = false;
        for (char prev_c : prev_word) {
            if (prev_c == c) { was_held = true; break; }
        }
        if (!was_held) {
            push(out, out_cap, count, ev_char(c));
        }
    }
    // Save current frame's word for next frame's comparison.
    prev_word = state.word;
    state.word.clear();

    return count;
}

// Human-readable names for logging.
const char* key_name(Key k) {
    switch (k) {
        case Key::NONE:      return "NONE";
        case Key::LEFT:      return "LEFT";
        case Key::RIGHT:     return "RIGHT";
        case Key::UP:        return "UP";
        case Key::DOWN:      return "DOWN";
        case Key::ENTER:     return "ENTER";
        case Key::ESC:       return "ESC";
        case Key::TAB:       return "TAB";
        case Key::BACKSPACE: return "BACKSPACE";
        case Key::SPACE:     return "SPACE";
        case Key::DELETE:    return "DELETE";
        case Key::CTRL_S:    return "CTRL_S";
        case Key::CTRL_Q:    return "CTRL_Q";
        case Key::CTRL_H:    return "CTRL_H";
    }
    return "?";
}

} // namespace ui
} // namespace nanojs8
