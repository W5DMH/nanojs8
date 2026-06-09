/*
 * screen_setup.cpp — NanoJS8 v0.7 SETUP screen (Layer 6b.3)
 * =============================================================
 * Operator configuration screen with edit mode. Same 6 rows as L6b.2
 * (Call / Grid / Groups / Units / Freq / Radio), but now individually
 * editable: click on the focused row to start typing, ENTER to save
 * or ESC to cancel.
 *
 * Edit-mode state machine
 * ───────────────────────
 *   IDLE  (s_editing_row == -1)
 *     • trackball UP/DOWN (after sensitivity divider) → move focus
 *     • trackball LEFT  → exit to HOME if station is configured
 *     • trackball CLICK → enter EDIT for focused row
 *     • keyboard ENTER  → enter EDIT for focused row
 *
 *   EDIT  (s_editing_row >= 0)
 *     • printable keyboard char → append to s_edit_buffer
 *     • BACKSPACE (0x08) → drop last char
 *     • keyboard ENTER (0x0D) or trackball CLICK → validate + commit
 *     • ESC (0x1B) → discard buffer, return to IDLE
 *     • trackball UP/DOWN/LEFT/RIGHT → ignored while editing (operator
 *       must commit/cancel before navigating)
 *
 * Trackball sensitivity divider
 * ─────────────────────────────
 * The T-Box optical encoders fire ~15-20 events per single physical
 * roll. Mapping 1 event → 1 row is far too twitchy. We accumulate
 * ticks per direction and only move focus when the threshold is hit;
 * the opposite direction resets the accumulator so users can reverse
 * mid-roll without "remembering" the previous bank.
 *
 * Saving
 * ──────
 * Committed values are normalized (callsign uppercased, grid prefix
 * uppercased + subsquare lowercased) then written through
 * nanojs8_config_set() and persisted via nanojs8_config_save(). Failures
 * are logged but don't crash — the in-memory cache still updates.
 *
 * License: GPL-3.0
 */

#include "ui.h"
#include "ui_internal.h"
#include "setup_validators.h"

#include "display.h"
#include "trackball.h"
#include "config.h"
#include "radio.h"   // L6b.4: lookup new profile on radio_id commit
#include "ptt.h"     // L6b.4: re-apply profile to PTT controller
#include "cat.h"     // L6b.5: re-apply profile to CAT facade
#include "time_source.h"  // L7.0: UTC clock for ROW_UTC commit

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "screen_setup";

namespace {

constexpr int SCREEN_W = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H   = 16;
constexpr int PAD_X    = 6;

constexpr int HEADER_Y    = 6;
constexpr int SEPARATOR_Y = HEADER_Y + FONT_H + 4;

constexpr int ROW_STRIDE = 24;
constexpr int ROW0_Y     = SEPARATOR_Y + 8;
constexpr int LABEL_X    = PAD_X + 4;
constexpr int VALUE_X    = PAD_X + 80;
constexpr int FOCUS_X    = 2;

constexpr int FOOTER_Y   = SCREEN_H - FONT_H - 6;

// Colors
constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER      = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_HEADER_EDIT = NANOJS8_COLOR_CYAN;  // header tint in edit
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_LABEL       = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_LABEL_FOC   = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_VALUE       = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_VALUE_BAD   = NANOJS8_COLOR_RED;
constexpr uint16_t COL_VALUE_DIM   = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_FOCUS_BG    = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_FOCUS_FG    = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_EDIT_BG     = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_EDIT_FG    = NANOJS8_COLOR_CYAN;     // chevron color
constexpr uint16_t COL_EDIT_TEXT   = NANOJS8_COLOR_CYAN;     // buffer color
constexpr uint16_t COL_EDIT_INVALID = NANOJS8_COLOR_RED;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;

// Rows — match MicroJS8's _setup_rows() order (mode/logs filtered out)
constexpr int NUM_ROWS = 7;             // L7.0: added ROW_UTC
constexpr int ROW_CALL   = 0;
constexpr int ROW_GRID   = 1;
constexpr int ROW_GROUPS = 2;
constexpr int ROW_UNITS  = 3;
constexpr int ROW_FREQ   = 4;
constexpr int ROW_RADIO  = 5;
constexpr int ROW_UTC    = 6;           // L7.0: manual UTC HH:MM:SS entry

// Per-row max edit length (excluding NUL terminator). Match the
// config struct's storage limits minus one.
// Safe truncating string copy. Used in commit_edit() to copy from
// s_edit_buffer (sized to the LARGEST per-field limit, NANOJS8_CONFIG_GROUPS_LEN)
// into the smaller per-field slots in nanojs8_config_t. The validators
// already ensure the buffer fits the destination, but GCC's
// -Werror=format-truncation can't see that connection through the
// abstraction and rejects snprintf("%s", ...) into a smaller buffer.
// Using memcpy with an explicit length sidesteps the warning entirely
// while preserving the safety property.
void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int row_max_len(int row) {
    switch (row) {
    case ROW_CALL:   return NANOJS8_CONFIG_CALLSIGN_LEN - 1;
    case ROW_GRID:   return NANOJS8_CONFIG_GRID_LEN - 1;
    case ROW_GROUPS: return NANOJS8_CONFIG_GROUPS_LEN - 1;
    case ROW_UNITS:  return NANOJS8_CONFIG_UNITS_LEN - 1;
    case ROW_FREQ:   return 15;     // "1234.567" plenty
    case ROW_RADIO:  return NANOJS8_CONFIG_RADIO_ID_LEN - 1;
    case ROW_UTC:    return 8;      // L7.0: "HH:MM:SS" or "HHMMSS"
    default:         return 0;
    }
}

// ── Focus + edit state ─────────────────────────────────────────────
int  s_focused_row          = ROW_CALL;
int  s_previous_focused_row = -1;       // for partial redraw

int  s_editing_row          = -1;       // -1 = idle; otherwise the row
char s_edit_buffer[NANOJS8_CONFIG_GROUPS_LEN] = {};
int  s_edit_pos             = 0;        // == strlen(s_edit_buffer)
bool s_edit_invalid         = false;

// L6b.4-hotfix2: picker state for ROW_RADIO. When editing the Radio
// row, we don't use s_edit_buffer at all — instead s_picker_index
// points into the radio registry, and trackball UP/DOWN cycles it.
// On commit we copy s_profiles[s_picker_index].id into the config.
int  s_picker_index         = 0;

// Predicate: should this row use the picker (cycle through fixed
// options) instead of the text editor? Centralizing this here lets
// every code path (init, render, handle_input, commit, footer) ask
// the same question.
bool row_uses_picker(int row) {
    return row == ROW_RADIO;
}

// Predicate: should chars typed into this row's text editor be
// auto-uppercased? JS8Call transmits these as caps anyway, and the
// T-Deck keyboard makes shift painful, so the operator types lower
// and we silently upper. Only applies to text-editor rows, not the
// picker.
bool row_auto_uppercases(int row) {
    return row == ROW_CALL || row == ROW_GRID || row == ROW_GROUPS;
}

// Trackball sensitivity accumulator. We count consecutive ticks in one
// direction; on direction change, the opposite counter resets. When a
// counter reaches FOCUS_TICK_THRESHOLD, we emit one focus move and
// reset the counter.
constexpr int FOCUS_TICK_THRESHOLD = 4;
int s_up_ticks   = 0;
int s_down_ticks = 0;
// L7.10: tick-debounce SETUP's idle-mode LEFT/RIGHT for screen-switch.
// Without this, a fast trackball flick would cascade through HOME and
// across to other screens before the operator could see the transition.
int s_left_ticks  = 0;
int s_right_ticks = 0;

// ── Helpers: per-row text generation (read-only mode) ──────────────

// Pretty-format the stored value for one row (when NOT editing).
// Writes into out (size out_len) and returns the appropriate color.
uint16_t row_stored_text(int row, char *out, size_t out_len) {
    const nanojs8_config_t *cfg = nanojs8_config_get();
    switch (row) {
    case ROW_CALL:
        snprintf(out, out_len, "%s", cfg->callsign);
        return (strcmp(cfg->callsign, NANOJS8_DEFAULT_CALLSIGN) == 0)
               ? COL_VALUE_BAD : COL_VALUE;
    case ROW_GRID:
        if (cfg->grid[0] == '\0') {
            snprintf(out, out_len, "(unset)");
            return COL_VALUE_BAD;
        }
        snprintf(out, out_len, "%s", cfg->grid);
        return COL_VALUE;
    case ROW_GROUPS:
        if (cfg->groups[0] == '\0') {
            snprintf(out, out_len, "(none)");
            return COL_VALUE_DIM;
        }
        snprintf(out, out_len, "%s", cfg->groups);
        return COL_VALUE;
    case ROW_UNITS:
        snprintf(out, out_len, "%s", cfg->units);
        return COL_VALUE;
    case ROW_FREQ: {
        double mhz = (double)cfg->freq_hz / 1000000.0;
        snprintf(out, out_len, "%.3f MHz", mhz);
        return COL_VALUE;
    }
    case ROW_RADIO: {
        // L6b.4-hotfix2: show the human-readable display_name, not the
        // canonical id. The picker writes the id on commit; the operator
        // never has to know or type the id-form again.
        const nanojs8_radio_profile_t *p =
            nanojs8_radio_lookup(cfg->radio_id);
        if (p) {
            snprintf(out, out_len, "%s", p->display_name);
        } else {
            // Unknown id (NVS stale) — show it raw so the operator sees
            // there's a mismatch, but the picker on edit will let them
            // recover by selecting a real profile.
            snprintf(out, out_len, "%s (?)", cfg->radio_id);
            return COL_VALUE_BAD;
        }
        return COL_VALUE;
    }
    case ROW_UTC: {
        // L7.0: show CURRENT UTC if the operator has entered one this
        // session, else a clear "not set" prompt. UTC is volatile —
        // not persisted in nanojs8_config_t (which the |cfg| arg
        // points to), so we read it from the time subsystem directly.
        uint8_t h = 0, m = 0, s = 0;
        if (nanojs8_time_get_utc(&h, &m, &s)) {
            snprintf(out, out_len, "%02u:%02u:%02uZ",
                     (unsigned)h, (unsigned)m, (unsigned)s);
            return COL_VALUE;
        }
        snprintf(out, out_len, "(not set)");
        return COL_VALUE_DIM;
    }
    default:
        snprintf(out, out_len, "?");
        return COL_VALUE_DIM;
    }
}

const char *row_label(int row) {
    switch (row) {
    case ROW_CALL:   return "Call";
    case ROW_GRID:   return "Grid";
    case ROW_GROUPS: return "Groups";
    case ROW_UNITS:  return "Units";
    case ROW_FREQ:   return "Freq";
    case ROW_RADIO:  return "Radio";
    case ROW_UTC:    return "UTC";
    default:         return "?";
    }
}

// Initialize the edit buffer with the current value of the row,
// formatted appropriately (e.g. freq as MHz decimal). For the picker
// row (ROW_RADIO) the buffer is ignored — we initialize s_picker_index
// instead so trackball UP/DOWN starts cycling from the current profile.
void init_edit_buffer(int row) {
    const nanojs8_config_t *cfg = nanojs8_config_get();

    if (row_uses_picker(row)) {
        // Picker mode: find current profile in the registry. If the
        // stored id is unknown (e.g. NVS stale), default to index 0 so
        // the operator can still cycle to a valid profile.
        int idx = nanojs8_radio_index_of(cfg->radio_id);
        if (idx < 0) idx = 0;
        s_picker_index = idx;
        s_edit_buffer[0] = '\0';
        s_edit_pos = 0;
        return;
    }

    switch (row) {
    case ROW_CALL:
        snprintf(s_edit_buffer, sizeof(s_edit_buffer), "%s", cfg->callsign);
        break;
    case ROW_GRID:
        snprintf(s_edit_buffer, sizeof(s_edit_buffer), "%s", cfg->grid);
        break;
    case ROW_GROUPS:
        snprintf(s_edit_buffer, sizeof(s_edit_buffer), "%s", cfg->groups);
        break;
    case ROW_UNITS:
        snprintf(s_edit_buffer, sizeof(s_edit_buffer), "%s", cfg->units);
        break;
    case ROW_FREQ: {
        // Display as plain decimal MHz, no unit suffix while editing.
        double mhz = (double)cfg->freq_hz / 1000000.0;
        snprintf(s_edit_buffer, sizeof(s_edit_buffer), "%.3f", mhz);
        break;
    }
    case ROW_UTC: {
        // L7.0: prefill with current UTC if set, else empty. We use
        // "HH:MM:SS" form so the operator sees the expected format
        // even when editing the existing value.
        uint8_t h = 0, m = 0, s = 0;
        if (nanojs8_time_get_utc(&h, &m, &s)) {
            snprintf(s_edit_buffer, sizeof(s_edit_buffer),
                     "%02u:%02u:%02u",
                     (unsigned)h, (unsigned)m, (unsigned)s);
        } else {
            s_edit_buffer[0] = '\0';
        }
        break;
    }
    default:
        s_edit_buffer[0] = '\0';
        break;
    }
    // L6b.4-hotfix2: for auto-cap rows, force the initial buffer to all
    // uppercase. The NVS values SHOULD already be normalized (we apply
    // upper on commit), but this guards against any case where they
    // aren't (a fresh field, a manual NVS poke, an old build's data).
    if (row_auto_uppercases(row)) {
        nanojs8_normalize_upper(s_edit_buffer);
    }
    s_edit_pos = (int)strlen(s_edit_buffer);
}

// Run the appropriate validator for the row currently being edited.
bool buffer_is_valid_for_row(int row, const char *buf) {
    switch (row) {
    case ROW_CALL:   return nanojs8_validate_callsign(buf);
    case ROW_GRID:   return nanojs8_validate_grid(buf);
    case ROW_GROUPS: return nanojs8_validate_groups(buf);
    case ROW_UNITS:  return nanojs8_validate_units(buf);
    case ROW_FREQ: {
        uint64_t hz;
        return nanojs8_parse_freq(buf, &hz);
    }
    case ROW_RADIO:  return nanojs8_validate_radio_id(buf);
    case ROW_UTC: {
        uint8_t h, m, s;
        return nanojs8_parse_utc(buf, &h, &m, &s);
    }
    default: return false;
    }
}

// Re-run the validator and update s_edit_invalid. The picker is always
// valid because s_picker_index can only ever point at registered profiles.
void revalidate_buffer() {
    if (row_uses_picker(s_editing_row)) {
        s_edit_invalid = false;
        return;
    }
    s_edit_invalid = !buffer_is_valid_for_row(s_editing_row, s_edit_buffer);
}

// ── Render helpers ─────────────────────────────────────────────────

// Paint a single row. is_focused → highlighted (idle focus or edit).
// When this row is being edited, we render the live buffer + cursor
// instead of the stored value.
void draw_row(int row) {
    int y = ROW0_Y + row * ROW_STRIDE;
    bool is_focused = (row == s_focused_row);
    bool is_editing = (row == s_editing_row);

    uint16_t row_bg = (is_focused || is_editing) ? COL_FOCUS_BG : COL_BG;
    nanojs8_display_fill_rect(0, y - 2, SCREEN_W, FONT_H + 4, row_bg);

    // Chevron — yellow when idle-focused, cyan while editing.
    if (is_editing) {
        nanojs8_display_draw_text(FOCUS_X, y, ">", COL_EDIT_FG, row_bg);
    } else if (is_focused) {
        nanojs8_display_draw_text(FOCUS_X, y, ">", COL_FOCUS_FG, row_bg);
    }

    // Label
    uint16_t label_color = (is_focused || is_editing) ?
                            COL_LABEL_FOC : COL_LABEL;
    nanojs8_display_draw_text(LABEL_X, y, row_label(row),
                               label_color, row_bg);

    // Value or live edit buffer
    if (is_editing) {
        if (row_uses_picker(row)) {
            // L6b.4-hotfix2: picker visual — show the currently selected
            // profile's display_name between < > cycle markers. No cursor,
            // no buffer.
            const nanojs8_radio_profile_t *p =
                nanojs8_radio_at((size_t)s_picker_index);
            char shown[40];
            if (p) {
                snprintf(shown, sizeof(shown), "< %.20s >", p->display_name);
            } else {
                snprintf(shown, sizeof(shown), "< ? >");
            }
            nanojs8_display_draw_text(VALUE_X, y, shown,
                                       COL_EDIT_TEXT, row_bg);
        } else {
            // Text editor: render buffer + cursor "_". Red on invalid.
            char shown[NANOJS8_CONFIG_GROUPS_LEN + 2];
            snprintf(shown, sizeof(shown), "%s_", s_edit_buffer);
            uint16_t color = s_edit_invalid ? COL_EDIT_INVALID : COL_EDIT_TEXT;
            nanojs8_display_draw_text(VALUE_X, y, shown, color, row_bg);
        }
    } else {
        char value[64];
        uint16_t value_color = row_stored_text(row, value, sizeof(value));
        nanojs8_display_draw_text(VALUE_X, y, value, value_color, row_bg);
    }
}

// Footer hint text depends on mode.
void draw_footer() {
    nanojs8_display_fill_rect(0, FOOTER_Y - 2, SCREEN_W, FONT_H + 4, COL_BG);

    const char *msg;
    if (s_editing_row >= 0) {
        if (row_uses_picker(s_editing_row)) {
            // L6b.4-hotfix2: picker has a different control set — no
            // typing, just cycling. Communicate that clearly.
            msg = "UP/DOWN choose  ENTER save  ESC/LEFT cancel";
        } else {
            msg = s_edit_invalid ?
                  "invalid - type to edit  ENTER save  ESC/LEFT cancel" :
                  "type value  ENTER save  ESC/LEFT cancel";
        }
    } else if (nanojs8_config_is_configured()) {
        msg = "TB up/down focus  CLICK edit  LEFT Home";
    } else {
        msg = "TB up/down focus  CLICK edit  Set Call+Grid first";
    }
    int w = nanojs8_display_text_width(msg);
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    nanojs8_display_draw_text(x, FOOTER_Y, msg, COL_FOOTER, COL_BG);
}

// Paint chrome on screen entry.
void paint_chrome() {
    nanojs8_display_clear(COL_BG);

    const char *hdr = (s_editing_row >= 0) ? "SETUP - edit" : "SETUP";
    uint16_t hdr_color = (s_editing_row >= 0) ? COL_HEADER_EDIT : COL_HEADER;
    int w = nanojs8_display_text_width(hdr);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, HEADER_Y, hdr,
                               hdr_color, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
    draw_footer();
}

// Repaint the header band only (called when mode toggles).
void repaint_header() {
    nanojs8_display_fill_rect(0, HEADER_Y - 2, SCREEN_W, FONT_H + 4, COL_BG);
    const char *hdr = (s_editing_row >= 0) ? "SETUP - edit" : "SETUP";
    uint16_t hdr_color = (s_editing_row >= 0) ? COL_HEADER_EDIT : COL_HEADER;
    int w = nanojs8_display_text_width(hdr);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, HEADER_Y, hdr,
                               hdr_color, COL_BG);
}

void on_enter() {
    ESP_LOGI(TAG, "Entering SETUP (focused row = %d)", s_focused_row);
    // Reset edit state on entry so we never re-show stale buffer.
    s_editing_row = -1;
    s_edit_invalid = false;
    s_edit_pos = 0;
    s_edit_buffer[0] = '\0';
    // L7.10: reset screen-switch tick debounce on entry so a flick
    // that just entered SETUP doesn't leave any partial counter state.
    s_left_ticks  = 0;
    s_right_ticks = 0;
    s_up_ticks = s_down_ticks = 0;
    s_previous_focused_row = -1;
    paint_chrome();
}

void render(bool full_redraw) {
    // We always redraw all rows since the focused row's highlight and
    // any edit cursor blinks need consistent refresh. Cost is modest:
    // 6 small fills + 6 short text draws over SPI at 40 MHz.
    (void)full_redraw;
    for (int i = 0; i < NUM_ROWS; ++i) draw_row(i);
    s_previous_focused_row = s_focused_row;
}

// ── Edit-mode helpers ──────────────────────────────────────────────

void enter_edit_mode(int row) {
    if (s_editing_row == row) return;
    s_editing_row = row;
    init_edit_buffer(row);
    revalidate_buffer();
    // Reset the tick accumulator so any leftover ticks from focus
    // navigation that brought us to this row don't bleed into the
    // picker's first cycle (or be lost when we exit edit mode).
    s_up_ticks = 0;
    s_down_ticks = 0;
    repaint_header();
    draw_footer();
    if (row_uses_picker(row)) {
        const nanojs8_radio_profile_t *p =
            nanojs8_radio_at((size_t)s_picker_index);
        ESP_LOGI(TAG, "Edit start: row=%d picker_idx=%d (%s)",
                 row, s_picker_index, p ? p->id : "?");
    } else {
        ESP_LOGI(TAG, "Edit start: row=%d initial='%s' invalid=%d",
                 row, s_edit_buffer, (int)s_edit_invalid);
    }
}

void cancel_edit() {
    if (s_editing_row < 0) return;
    ESP_LOGI(TAG, "Edit cancel on row %d", s_editing_row);
    s_editing_row = -1;
    s_edit_buffer[0] = '\0';
    s_edit_pos = 0;
    s_edit_invalid = false;
    s_up_ticks = 0;
    s_down_ticks = 0;
    repaint_header();
    draw_footer();
}

// Commit the edit buffer to the config. Returns true on success,
// false if validation failed (caller stays in edit mode).
bool commit_edit() {
    if (s_editing_row < 0) return false;
    if (s_edit_invalid) {
        ESP_LOGW(TAG, "Commit blocked: buffer fails validation");
        return false;
    }

    int row = s_editing_row;

    // L7.0: UTC commit is special — it does NOT persist to NVS
    // (operator must re-enter every session by design; the time
    // anchor is volatile). Handle it before the standard config
    // path so we don't trigger a no-op NVS write.
    if (row == ROW_UTC) {
        uint8_t h = 0, m = 0, s = 0;
        if (!nanojs8_parse_utc(s_edit_buffer, &h, &m, &s)) {
            ESP_LOGW(TAG, "Commit blocked: UTC parse failed after validation");
            return false;
        }
        esp_err_t err = nanojs8_time_set_utc(h, m, s);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nanojs8_time_set_utc failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "Edit commit: row=%d UTC=%02u:%02u:%02u (volatile)",
                 row, (unsigned)h, (unsigned)m, (unsigned)s);
        s_editing_row = -1;
        s_edit_buffer[0] = '\0';
        s_edit_pos = 0;
        s_edit_invalid = false;
        s_up_ticks = 0;
        s_down_ticks = 0;
        repaint_header();
        draw_footer();
        return true;
    }

    // Take a working copy of the current config so we can mutate one
    // field without disturbing the others, then push the whole thing
    // through set() + save().
    nanojs8_config_t new_cfg;
    memcpy(&new_cfg, nanojs8_config_get(), sizeof(new_cfg));

    switch (row) {
    case ROW_CALL:
        nanojs8_normalize_upper(s_edit_buffer);
        safe_copy(new_cfg.callsign, sizeof(new_cfg.callsign), s_edit_buffer);
        break;
    case ROW_GRID:
        nanojs8_normalize_grid(s_edit_buffer);
        safe_copy(new_cfg.grid, sizeof(new_cfg.grid), s_edit_buffer);
        break;
    case ROW_GROUPS:
        // Groups: leave case as typed (case-sensitive group tags)
        safe_copy(new_cfg.groups, sizeof(new_cfg.groups), s_edit_buffer);
        break;
    case ROW_UNITS:
        // Normalize lowercase
        for (char *p = s_edit_buffer; *p; ++p) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }
        safe_copy(new_cfg.units, sizeof(new_cfg.units), s_edit_buffer);
        break;
    case ROW_FREQ: {
        uint64_t hz = 0;
        if (!nanojs8_parse_freq(s_edit_buffer, &hz)) {
            ESP_LOGW(TAG, "Commit blocked: freq parse failed after validation");
            return false;
        }
        new_cfg.freq_hz = hz;
        break;
    }
    case ROW_RADIO: {
        // L6b.4-hotfix2: picker mode — pull the id from the selected
        // registry entry. The text buffer is unused for this row.
        const nanojs8_radio_profile_t *p =
            nanojs8_radio_at((size_t)s_picker_index);
        if (!p) {
            ESP_LOGE(TAG, "Commit blocked: picker index %d out of range",
                     s_picker_index);
            return false;
        }
        safe_copy(new_cfg.radio_id, sizeof(new_cfg.radio_id), p->id);
        break;
    }
    }

    esp_err_t set_err = nanojs8_config_set(&new_cfg);
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "config_set failed: %s", esp_err_to_name(set_err));
        return false;
    }
    esp_err_t save_err = nanojs8_config_save();
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "config_save failed: %s (in-memory cache still updated)",
                 esp_err_to_name(save_err));
        // Continue — operator's change is at least live in this session
    }

    // Log message: picker rows show the resolved id, text rows show buffer.
    if (row_uses_picker(row)) {
        ESP_LOGI(TAG, "Edit commit: row=%d picker=%s saved",
                 row, new_cfg.radio_id);
    } else {
        ESP_LOGI(TAG, "Edit commit: row=%d value='%s' saved",
                 row, s_edit_buffer);
    }

    // L6b.4: if the radio profile changed, push the new profile to the
    // PTT controller so it re-configures the serial PTT line. We do
    // this AFTER the config write so a failed save doesn't leave the
    // PTT layer pointed at a profile that wasn't actually persisted.
    // L6b.5: also push to the CAT facade — that updates the CI-V
    // addresses, resets the parser, and changes serial baud as needed.
    if (row == ROW_RADIO) {
        const nanojs8_radio_profile_t *new_profile =
            nanojs8_radio_lookup(new_cfg.radio_id);
        if (new_profile) {
            esp_err_t pe = nanojs8_ptt_apply_profile(new_profile);
            if (pe != ESP_OK && pe != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "ptt_apply_profile failed: %s",
                         esp_err_to_name(pe));
            }
            nanojs8_cat_apply_profile(new_profile);
        }
        // No else — the validator already guarantees the id is in the
        // registry, so lookup() returning NULL would be a logic bug.
    }

    // L6b.6: if the FREQ row was committed AND the active radio
    // profile speaks CI-V AND it advertises set_freq support, push
    // the new freq to the radio. This is the round-trip test path:
    // operator edits freq in SETUP → we save locally → we send
    // FE FE 70 E0 05 <BCD freq> FD to the radio → the radio echoes
    // the new freq back via the same 0x05 command → the CAT facade
    // updates s_last_freq_hz → HOME radio row shows the new value.
    // End-to-end verification of both directions in one user action.
    //
    // No "F" hotkey is needed for read probes anymore — apply_profile
    // already queues one via cat_tick, and the SETUP commit covers the
    // operator-driven case. This deliberately replaces the previous
    // L6b.5 manual-probe trigger (F / trackball CLICK / trackball UP).
    if (row == ROW_FREQ) {
        const nanojs8_radio_profile_t *active = nanojs8_radio_get_active();
        if (active && active->cat == NANOJS8_RADIO_CAT_CIV &&
            active->can_set_freq) {
            bool sent = nanojs8_cat_set_freq(new_cfg.freq_hz);
            if (sent) {
                ESP_LOGI(TAG, "CAT set_freq(%llu Hz) queued via SETUP commit",
                         (unsigned long long)new_cfg.freq_hz);
            } else {
                ESP_LOGW(TAG, "CAT set_freq did not send (serial not ready?) "
                              "— freq saved locally; reconnect DigiRig and "
                              "edit again to retry");
            }
        }
    }

    s_editing_row = -1;
    s_edit_buffer[0] = '\0';
    s_edit_pos = 0;
    s_edit_invalid = false;
    s_up_ticks = 0;
    s_down_ticks = 0;
    repaint_header();
    draw_footer();
    return true;
}

// ── Idle-mode trackball helpers ─────────────────────────────────────

void focus_move(int delta) {
    s_focused_row = (s_focused_row + delta + NUM_ROWS) % NUM_ROWS;
    ESP_LOGI(TAG, "Focus -> row %d", s_focused_row);
}

// Process a trackball direction tick. Updates accumulator; emits one
// focus move when threshold is hit; resets opposite-direction counter.
void tick_up() {
    s_down_ticks = 0;
    if (++s_up_ticks >= FOCUS_TICK_THRESHOLD) {
        s_up_ticks = 0;
        focus_move(-1);
    }
}
void tick_down() {
    s_up_ticks = 0;
    if (++s_down_ticks >= FOCUS_TICK_THRESHOLD) {
        s_down_ticks = 0;
        focus_move(+1);
    }
}

// ── Input handler ──────────────────────────────────────────────────

bool handle_input(uint8_t event) {
    // EDIT MODE
    if (s_editing_row >= 0) {
        // LEFT cancels in both modes — natural "back" gesture, also the
        // failsafe escape if the keyboard is unresponsive (see L6b.3
        // hotfix1 notes).
        if (event == NANOJS8_TRACKBALL_LEFT) {
            cancel_edit();
            return true;
        }

        // ── Picker path (ROW_RADIO) ─────────────────────────────────
        if (row_uses_picker(s_editing_row)) {
            // Trackball UP/DOWN cycle through registered profiles using
            // the SAME tick-divider as idle-mode focus navigation. One
            // deliberate click-up gesture (~5-8 trackball events) =
            // one cycle step. Without this, a single roll would whip
            // through the 3-entry list ~5 times and the displayed
            // profile would be effectively random by the time the
            // operator stopped rolling.
            size_t count = nanojs8_radio_count();
            if (count == 0) {
                // Defensive: a registry with no entries shouldn't be
                // possible (compile-time array, must have at least one),
                // but guard against future bugs.
                cancel_edit();
                return true;
            }
            if (event == NANOJS8_TRACKBALL_UP) {
                s_down_ticks = 0;
                if (++s_up_ticks >= FOCUS_TICK_THRESHOLD) {
                    s_up_ticks = 0;
                    s_picker_index =
                        (s_picker_index - 1 + (int)count) % (int)count;
                }
                return true;
            }
            if (event == NANOJS8_TRACKBALL_DOWN) {
                s_up_ticks = 0;
                if (++s_down_ticks >= FOCUS_TICK_THRESHOLD) {
                    s_down_ticks = 0;
                    s_picker_index = (s_picker_index + 1) % (int)count;
                }
                return true;
            }
            // CLICK or ENTER commit
            if (event == NANOJS8_TRACKBALL_CLICK || event == 0x0D) {
                commit_edit();
                return true;
            }
            // ESC cancels
            if (event == 0x1B) {
                cancel_edit();
                return true;
            }
            // Picker doesn't care about printable chars, BS, RIGHT.
            // Swallow them to avoid surprising side effects.
            return true;
        }

        // ── Text editor path (all other rows) ────────────────────────
        // UP/DOWN/RIGHT swallowed while editing — no nav semantics
        // mid-edit.
        if (event == NANOJS8_TRACKBALL_UP ||
            event == NANOJS8_TRACKBALL_DOWN ||
            event == NANOJS8_TRACKBALL_RIGHT) {
            return true;
        }
        // Trackball CLICK commits (same as ENTER)
        if (event == NANOJS8_TRACKBALL_CLICK) {
            commit_edit();
            return true;
        }
        switch (event) {
        case 0x0D:   // ENTER
            commit_edit();
            return true;
        case 0x1B:   // ESC
            cancel_edit();
            return true;
        case 0x08: { // BACKSPACE
            if (s_edit_pos > 0) {
                s_edit_buffer[--s_edit_pos] = '\0';
                revalidate_buffer();
            }
            return true;
        }
        default: break;
        }
        // Printable: append (if room). For auto-cap rows, lowercase
        // letters are silently uppercased on the way in — JS8Call wants
        // caps and shifting on the tiny keyboard is painful. Digits,
        // slash, hyphen, comma, '@' all pass through unchanged; only
        // 'a'..'z' get the transform.
        if (event >= 0x20 && event <= 0x7E) {
            char c = (char)event;
            if (row_auto_uppercases(s_editing_row) &&
                c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            int max = row_max_len(s_editing_row);
            if (s_edit_pos < max &&
                (size_t)s_edit_pos < sizeof(s_edit_buffer) - 1) {
                s_edit_buffer[s_edit_pos++] = c;
                s_edit_buffer[s_edit_pos]   = '\0';
                revalidate_buffer();
            }
            return true;
        }
        return false;
    }

    // IDLE MODE
    switch (event) {
    case NANOJS8_TRACKBALL_UP:    tick_up();   return true;
    case NANOJS8_TRACKBALL_DOWN:  tick_down(); return true;
    case NANOJS8_TRACKBALL_LEFT:
        s_right_ticks = 0;
        if (++s_left_ticks < FOCUS_TICK_THRESHOLD) {
            return true;  // accumulating — not yet a confirmed press
        }
        s_left_ticks = 0;
        if (nanojs8_config_is_configured()) {
            nanojs8_ui_set_screen(NANOJS8_SCREEN_HOME);
        } else {
            ESP_LOGI(TAG, "  Refusing to leave SETUP: callsign or grid still unset");
        }
        return true;
    case NANOJS8_TRACKBALL_RIGHT:
        // L7.9: SETUP → HEARD is the next step in the right-cycle.
        // L7.10: tick-debounced to suppress a single trackball flick
        // from chaining across to ALL or DIRECTED.
        s_left_ticks = 0;
        if (++s_right_ticks < FOCUS_TICK_THRESHOLD) {
            return true;
        }
        s_right_ticks = 0;
        if (nanojs8_config_is_configured()) {
            nanojs8_ui_set_screen(NANOJS8_SCREEN_HEARD);
        } else {
            ESP_LOGI(TAG, "  Refusing to leave SETUP: callsign or grid still unset");
        }
        return true;
    case NANOJS8_TRACKBALL_CLICK:
    case 0x0D:   // ENTER also enters edit
        enter_edit_mode(s_focused_row);
        return true;
    default:
        return false;
    }
}

} // anonymous namespace

// Exported screen descriptor — see SCREEN_HOME's comment for why `extern`.
extern const nanojs8_screen_t SCREEN_SETUP = {
    .id           = NANOJS8_SCREEN_SETUP,
    .name         = "SETUP",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
