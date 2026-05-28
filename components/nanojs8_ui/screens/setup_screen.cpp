// NanoJS8 — SETUP screen implementation.

#include "screens/setup_screen.h"

#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include <M5Cardputer.h>

#include "config_store.h"

namespace nanojs8 {
namespace ui {
namespace screens {

static const char* TAG = "setup";

// Layout constants — derived from the 240×135 Cardputer ADV display.
// All positions in pixels. Hand-tuned for the M5GFX default font at
// the indicated sizes.
namespace layout {
    constexpr int SCREEN_W = 240;
    constexpr int SCREEN_H = 135;

    constexpr int HEADER_Y      = 4;
    constexpr int HEADER_H      = 16;
    constexpr int FIELDS_Y      = 28;
    constexpr int FIELD_ROW_H   = 16;
    constexpr int LABEL_X       = 12;
    constexpr int VALUE_X       = 80;
    constexpr int FOCUS_MARKER_X = 2;
    constexpr int FOOTER_Y      = 116;

    // Banner overlay (covers center of screen briefly).
    constexpr int BANNER_Y      = 60;
    constexpr int BANNER_H      = 20;
    constexpr int BANNER_X      = 20;
    constexpr int BANNER_W      = 200;

    // Banner display duration (milliseconds).
    constexpr uint32_t BANNER_MS = 1500;
}

// Field labels — exactly as confirmed by the operator.
static const char* FIELD_LABELS[] = {
    "CALL  ",
    "GRID  ",
    "RADIO ",
    "GROUPS",
    "AUTOST",   // "Auto-start radio service on boot" - truncated to fit 6-char column
};

// Pull current millis (no overflow concern over our use window).
static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Per-field max length (for edit_buf_ overflow protection at typing
// time — once the operator has typed this many chars, additional
// keystrokes are dropped silently. The validator's bounds are the
// authoritative check at commit time; this is just to give immediate
// "no more room" feedback.)
static uint8_t field_max_len(SetupScreen::Field f) {
    switch (f) {
        case SetupScreen::Field::CALL:      return NANOJS8_CALLSIGN_MAXLEN - 1;
        case SetupScreen::Field::GRID:      return NANOJS8_GRID_MAXLEN     - 1;
        case SetupScreen::Field::RADIO:     return NANOJS8_RADIO_MAXLEN    - 1;
        case SetupScreen::Field::GROUPS:    return NANOJS8_GROUPS_MAXLEN   - 1;
        case SetupScreen::Field::AUTOSTART: return 3;  // "on" or "off"
        case SetupScreen::Field::COUNT:     break;
    }
    return 19;
}

// Get the currently-committed value for a field, as a const string.
static const char* committed_value(SetupScreen::Field f) {
    const Config& cfg = nanojs8::config::current();
    switch (f) {
        case SetupScreen::Field::CALL:      return cfg.callsign;
        case SetupScreen::Field::GRID:      return cfg.grid;
        case SetupScreen::Field::RADIO:     return cfg.radio;
        case SetupScreen::Field::GROUPS:    return cfg.groups;
        case SetupScreen::Field::AUTOSTART: return cfg.radio_autostart ? "on" : "off";
        case SetupScreen::Field::COUNT:     break;
    }
    return "";
}

// Commit edit_buf_ contents to the focused field. Returns true on
// success; false if validation rejected.
static bool try_commit_field(SetupScreen::Field f, const char* draft) {
    using namespace nanojs8::config;
    switch (f) {
        case SetupScreen::Field::CALL:   return set_callsign(draft) == ESP_OK;
        case SetupScreen::Field::GRID:   return set_grid(draft)     == ESP_OK;
        case SetupScreen::Field::RADIO:  return set_radio(draft)    == ESP_OK;
        case SetupScreen::Field::GROUPS: return set_groups(draft)   == ESP_OK;
        case SetupScreen::Field::AUTOSTART:
            // The menu cycle already constrains draft to "on" or "off";
            // any other value is a bug.
            if (draft && std::strcmp(draft, "on") == 0)  return set_radio_autostart(true)  == ESP_OK;
            if (draft && std::strcmp(draft, "off") == 0) return set_radio_autostart(false) == ESP_OK;
            return false;
        case SetupScreen::Field::COUNT:  break;
    }
    return false;
}

// -------------------------------------------------------------------------
// Constructor / on_enter
// -------------------------------------------------------------------------

SetupScreen::SetupScreen()
    : focus_(Field::CALL)
    , mode_(Mode::NAV)
    , dirty_(false)
    , edit_len_(0)
    , show_error_(false)
    , show_saved_(false)
    , banner_clear_at_ms_(0)
    , needs_full_redraw_(true)
{
    edit_buf_[0] = '\0';
}

void SetupScreen::on_enter() {
    focus_ = Field::CALL;
    mode_  = Mode::NAV;
    edit_len_ = 0;
    edit_buf_[0] = '\0';
    show_error_ = false;
    show_saved_ = false;
    needs_full_redraw_ = true;
    ESP_LOGI(TAG, "SETUP screen entered");
}

bool SetupScreen::is_editing_text() const {
    // Only suppress ring-nav arrows when actively editing a *text*
    // field. NAV mode allows ring nav freely; menu-field edit mode
    // (RADIO) lets the router translate bare `;`/`.` to UP/DOWN
    // (which would otherwise have been typeable chars). Only text-
    // field edit (CALL, GRID, GROUPS) needs the `,` `/` chars to
    // pass through as typed input.
    return mode_ == Mode::EDIT && !field_is_menu(focus_);
}

// -------------------------------------------------------------------------
// Mode transitions
// -------------------------------------------------------------------------

bool SetupScreen::field_is_menu(Field f) const {
    return f == Field::RADIO || f == Field::AUTOSTART;
}

void SetupScreen::enter_edit_mode() {
    // Snapshot the committed value into edit_buf_ so the user can edit
    // in place. For menu fields, edit mode is also where UP/DOWN
    // cycle options.
    const char* current = committed_value(focus_);
    const size_t len = std::strlen(current);
    const size_t max = sizeof(edit_buf_) - 1;
    const size_t copy_len = (len < max) ? len : max;
    std::memcpy(edit_buf_, current, copy_len);
    edit_buf_[copy_len] = '\0';
    edit_len_ = (uint8_t)copy_len;
    mode_ = Mode::EDIT;
    needs_full_redraw_ = true;
    ESP_LOGD(TAG, "EDIT mode on field %d, draft=%s", (int)focus_, edit_buf_);
}

void SetupScreen::commit_edit_mode() {
    edit_buf_[edit_len_] = '\0';
    if (try_commit_field(focus_, edit_buf_)) {
        // Success — exit edit mode, mark dirty so Ctrl+S has something
        // to flush.
        mode_ = Mode::NAV;
        dirty_ = true;
        show_error_ = false;
        needs_full_redraw_ = true;
        ESP_LOGI(TAG, "Field %d committed: %s", (int)focus_, edit_buf_);
    } else {
        // Validation failed — stay in edit mode, show error banner.
        show_error_ = true;
        banner_clear_at_ms_ = now_ms() + layout::BANNER_MS;
        needs_full_redraw_ = true;
        ESP_LOGW(TAG, "Field %d commit rejected: %s", (int)focus_, edit_buf_);
    }
}

void SetupScreen::cancel_edit_mode() {
    mode_ = Mode::NAV;
    edit_len_ = 0;
    edit_buf_[0] = '\0';
    show_error_ = false;
    needs_full_redraw_ = true;
    ESP_LOGD(TAG, "EDIT cancelled, reverted to committed value");
}

void SetupScreen::cycle_focus_forward() {
    if (mode_ == Mode::EDIT) {
        // Tab while editing: commit-then-advance. If commit fails, stay
        // put with the error banner (Q3 = Option B).
        commit_edit_mode();
        if (mode_ == Mode::EDIT) {
            return;  // commit was rejected; don't advance
        }
    }
    uint8_t next = ((uint8_t)focus_ + 1) % (uint8_t)Field::COUNT;
    focus_ = (Field)next;
    needs_full_redraw_ = true;
    ESP_LOGD(TAG, "Focus → field %d", (int)focus_);
}

void SetupScreen::cycle_radio_option(bool forward) {
    using namespace nanojs8::config;
    // Find current radio in the catalog.
    size_t idx = 0;
    for (size_t i = 0; i < NANOJS8_RADIO_PROFILES_COUNT; ++i) {
        if (std::strcmp(edit_buf_, NANOJS8_RADIO_PROFILES[i]) == 0) {
            idx = i;
            break;
        }
    }
    if (forward) {
        idx = (idx + 1) % NANOJS8_RADIO_PROFILES_COUNT;
    } else {
        idx = (idx == 0) ? (NANOJS8_RADIO_PROFILES_COUNT - 1) : (idx - 1);
    }
    const size_t max = sizeof(edit_buf_) - 1;
    const size_t newlen = std::strlen(NANOJS8_RADIO_PROFILES[idx]);
    const size_t copy_len = (newlen < max) ? newlen : max;
    std::memcpy(edit_buf_, NANOJS8_RADIO_PROFILES[idx], copy_len);
    edit_buf_[copy_len] = '\0';
    edit_len_ = (uint8_t)copy_len;
    needs_full_redraw_ = true;
}

void SetupScreen::cycle_autostart_option(bool forward) {
    // Boolean toggle. Direction is irrelevant — both UP and DOWN flip.
    (void)forward;
    const bool was_on = (std::strcmp(edit_buf_, "on") == 0);
    const char* next = was_on ? "off" : "on";
    const size_t newlen = std::strlen(next);
    std::memcpy(edit_buf_, next, newlen + 1);  // include NUL
    edit_len_ = (uint8_t)newlen;
    needs_full_redraw_ = true;
}

bool SetupScreen::save_all() {
    esp_err_t err = nanojs8::config::save();
    if (err == ESP_OK) {
        dirty_ = false;
        show_saved_ = true;
        show_error_ = false;
        banner_clear_at_ms_ = now_ms() + layout::BANNER_MS;
        needs_full_redraw_ = true;
        return true;
    }
    show_error_ = true;
    banner_clear_at_ms_ = now_ms() + layout::BANNER_MS;
    needs_full_redraw_ = true;
    ESP_LOGE(TAG, "save_all: NVS save failed: %s", esp_err_to_name(err));
    return false;
}

// -------------------------------------------------------------------------
// Event dispatch
// -------------------------------------------------------------------------

bool SetupScreen::handle_event(const InputEvent& ev) {
    // Ctrl+S works in any mode.
    if (ev.key == Key::CTRL_S) {
        save_all();
        return true;
    }

    if (mode_ == Mode::NAV) {
        switch (ev.key) {
            case Key::TAB:
                cycle_focus_forward();
                return true;
            case Key::ENTER:
                enter_edit_mode();
                return true;
            case Key::UP:
            case Key::DOWN:
                // UP/DOWN in NAV mode could cycle field focus (some
                // form UIs do this) but MicroJS8 reserves them for
                // menu-option cycling within an edit. To match that
                // grammar exactly, we ignore UP/DOWN in NAV mode and
                // let Tab be the sole field-cycling key.
                return true;
            case Key::LEFT:
            case Key::RIGHT:
                // Screen-ring navigation. Defer to router by returning
                // false; router will log Phase 1's stub message.
                return false;
            default:
                break;
        }
        return false;
    }

    // Mode::EDIT
    if (field_is_menu(focus_)) {
        // Menu field — RADIO or AUTOSTART. UP/DOWN cycle options,
        // Enter commits, Esc cancels. Typed chars ignored.
        // Bare semicolon and period as UP/DOWN aliases. The Cardputer
        // ADV has no physical arrow keys; in MicroJS8's UART firmware
        // these chars map to UP/DOWN events. Mirror that here for
        // menu-field editing so the operator doesn't need to hold Fn.
        auto cycle = [this](bool forward) {
            if (focus_ == Field::RADIO) {
                cycle_radio_option(forward);
            } else if (focus_ == Field::AUTOSTART) {
                cycle_autostart_option(forward);
            }
        };
        if (ev.ch == ';') {
            cycle(false);  // ';' = up
            return true;
        }
        if (ev.ch == '.') {
            cycle(true);   // '.' = down
            return true;
        }
        switch (ev.key) {
            case Key::UP:
                cycle(false);
                return true;
            case Key::DOWN:
                cycle(true);
                return true;
            case Key::ENTER:
                commit_edit_mode();
                return true;
            case Key::ESC:
                cancel_edit_mode();
                return true;
            case Key::TAB:
                cycle_focus_forward();  // commits then advances
                return true;
            default:
                break;
        }
        // Typed chars on a menu field: ignore (no-op).
        return true;
    }

    // Text field — CALL, GRID. Typed chars append, Backspace deletes,
    // Enter commits, Esc cancels.
    switch (ev.key) {
        case Key::BACKSPACE:
            if (edit_len_ > 0) {
                edit_len_--;
                edit_buf_[edit_len_] = '\0';
                needs_full_redraw_ = true;
            }
            return true;
        case Key::ENTER:
            commit_edit_mode();
            return true;
        case Key::ESC:
            cancel_edit_mode();
            return true;
        case Key::TAB:
            cycle_focus_forward();
            return true;
        case Key::UP:
        case Key::DOWN:
            // Text fields don't respond to UP/DOWN; harmlessly consumed.
            return true;
        case Key::LEFT:
        case Key::RIGHT:
            // While editing, screen-ring nav is suppressed — operator
            // must Esc or Enter first. Otherwise an accidental Fn+,
            // would lose unsaved edits.
            return true;
        default:
            break;
    }

    // Printable char typed: append if room. Auto-uppercase ASCII letters
    // so the operator doesn't need to hold Shift (JS8 / amateur radio
    // convention — all callsigns and grids are uppercase, even though
    // grid subsquares are conventionally lowercase, which the validator
    // canonicalizes on commit). Digits, slash, and other chars pass
    // through unchanged so callsigns like "W5DMH" and grids like "EM10"
    // type naturally.
    if (ev.ch != 0) {
        const uint8_t max = field_max_len(focus_);
        if (edit_len_ < max) {
            char c = ev.ch;
            if (c >= 'a' && c <= 'z') {
                c = char(c - 'a' + 'A');
            }
            edit_buf_[edit_len_++] = c;
            edit_buf_[edit_len_] = '\0';
            needs_full_redraw_ = true;
        }
        return true;
    }

    return false;
}

// -------------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------------

void SetupScreen::render() {
    // Clear banner state on timeout.
    if ((show_error_ || show_saved_) && now_ms() >= banner_clear_at_ms_) {
        show_error_ = false;
        show_saved_ = false;
        needs_full_redraw_ = true;
    }

    if (!needs_full_redraw_) {
        return;
    }
    draw_full();
    if (show_error_ || show_saved_) {
        draw_banner();
    }
    needs_full_redraw_ = false;
}

void SetupScreen::draw_full() {
    auto& d = M5Cardputer.Display;

    d.fillScreen(TFT_BLACK);

    // Header.
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(layout::LABEL_X, layout::HEADER_Y);
    d.printf("SETUP");

    // Dirty marker on header right side if config has unsaved changes.
    if (dirty_) {
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setTextSize(1);
        d.setCursor(layout::SCREEN_W - 30, layout::HEADER_Y + 4);
        d.printf("*");
    }

    // Field rows.
    d.setTextSize(1);
    for (int i = 0; i < (int)Field::COUNT; ++i) {
        const int y = layout::FIELDS_Y + i * layout::FIELD_ROW_H;
        const bool is_focused = ((Field)i == focus_);
        const bool is_editing = is_focused && (mode_ == Mode::EDIT);

        // Focus marker (▸ on the focused row).
        if (is_focused) {
            d.setTextColor(is_editing ? TFT_YELLOW : TFT_GREEN, TFT_BLACK);
            d.setCursor(layout::FOCUS_MARKER_X, y);
            d.printf(">");
        }

        // Label.
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(layout::LABEL_X, y);
        d.printf("%s:", FIELD_LABELS[i]);

        // Value: committed if not editing this row, draft if editing.
        const char* value = is_editing ? edit_buf_ : committed_value((Field)i);
        if (is_editing) {
            d.setTextColor(TFT_YELLOW, TFT_BLACK);
        } else if (is_focused) {
            d.setTextColor(TFT_CYAN, TFT_BLACK);
        } else {
            d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(layout::VALUE_X, y);
        d.printf("%s", value);

        // Cursor caret when editing a text field.
        if (is_editing && !field_is_menu((Field)i)) {
            d.setTextColor(TFT_YELLOW, TFT_BLACK);
            d.printf("_");
        }

        // Menu-field indicator (◂ ▸) when editing a menu.
        if (is_editing && field_is_menu((Field)i)) {
            d.setTextColor(TFT_DARKGREY, TFT_BLACK);
            d.printf(" <>");
        }
    }

    // Footer hint — context-sensitive.
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(layout::LABEL_X, layout::FOOTER_Y);
    if (mode_ == Mode::NAV) {
        d.printf("Tab next  Enter edit  ^S save");
    } else if (field_is_menu(focus_)) {
        d.printf("Fn+;/. cycle  Enter ok  Fn+` esc");
    } else {
        d.printf("type  Bksp del  Enter ok  Fn+` esc");
    }
}

void SetupScreen::draw_banner() {
    auto& d = M5Cardputer.Display;

    const uint16_t bg = show_error_ ? TFT_RED : TFT_DARKGREEN;
    d.fillRoundRect(layout::BANNER_X, layout::BANNER_Y,
                    layout::BANNER_W, layout::BANNER_H, 4, bg);
    d.drawRoundRect(layout::BANNER_X, layout::BANNER_Y,
                    layout::BANNER_W, layout::BANNER_H, 4, TFT_WHITE);

    d.setTextColor(TFT_WHITE, bg);
    d.setTextSize(1);
    const char* msg = show_error_ ? "Invalid value" : "Saved";
    // Crude centering: assume 6 px/char at size 1.
    const int text_w = (int)std::strlen(msg) * 6;
    const int text_x = layout::BANNER_X + (layout::BANNER_W - text_w) / 2;
    const int text_y = layout::BANNER_Y + 6;
    d.setCursor(text_x, text_y);
    d.printf("%s", msg);
}

} // namespace screens
} // namespace ui
} // namespace nanojs8
