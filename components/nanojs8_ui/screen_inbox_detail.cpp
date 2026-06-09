/*
 * screen_inbox_detail.cpp — L7.11g.3 INBOX_DETAIL modal screen
 * =============================================================
 *
 * Full-text view of a single mailbox entry, reached via ENTER on the
 * INBOX list screen. Off the main ring — exited via BACKSPACE (0x08)
 * or TRACKBALL_LEFT, which returns to INBOX.
 *
 * Layout (320×240 landscape, 8×16 font, 40 char-columns):
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ MSG #14                              UNREAD          │  header: id + type
 *   │ ────────────────────────────────────────              │
 *   │ From:  KB1MCT                                         │
 *   │ To:    W5DMH                                          │  kv block
 *   │ At:    18:42:15                                       │
 *   │ SNR:   -09 dB                                         │
 *   │ Freq:  573 Hz                                         │
 *   │ ────────────────────────────────────────              │
 *   │                                                       │
 *   │ Hello there how have you been today.                  │  body
 *   │ I tried to call you on 20m yesterday                  │  wrapped
 *   │ but no joy.                                           │  42 chars
 *   │                                                       │
 *   │ ────────────────────────────────────────              │
 *   │ BACKSPACE  return                                     │  footer
 *   └──────────────────────────────────────────────────────┘
 *
 * Entry capture: when INBOX's handle_input transitions to us, the
 * selected entry's id is fetched via nanojs8_screen_inbox_selected_id()
 * and the corresponding entry is copied into s_entry on on_enter().
 * If the entry vanishes between INBOX's mark_read() and our render
 * (background task deletion etc.), we paint a "no longer available"
 * notice and BACKSPACE returns to INBOX as usual.
 *
 * Body wrapping is 38 chars/line (matches INBOX preview width: PAD=4
 * gives a 312-px usable area = 39 char advances, so 38 chars + final
 * trailing pad is safe). Wraps at word boundaries when possible.
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "mailbox.h"
#include "config.h"      // L7.11g.7: remote-store detection for RELAY label
#include "time_source.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>     // L7.11g.7: strcasecmp

extern "C" uint16_t nanojs8_screen_inbox_selected_id(void);

namespace {

constexpr const char *TAG = "screen_inbox_dt";

// ── Layout ───────────────────────────────────────────────────────────
constexpr int SCREEN_W = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H   = 16;
constexpr int FONT_W   = 8;
constexpr int PAD_X    = 4;

constexpr int HEADER_Y     = 4;
constexpr int SEPARATOR1_Y = HEADER_Y + FONT_H + 2;            // 22

// KV block: 5 rows × (FONT_H + 2 px gap) = 5 × 18 = 90.
constexpr int KV_FIRST_Y   = SEPARATOR1_Y + 4;                  // 26
constexpr int KV_STRIDE    = FONT_H + 2;                        // 18
constexpr int KV_LABEL_X   = PAD_X;
constexpr int KV_VALUE_X   = PAD_X + 7 * FONT_W;                // 60 — "Freq:  " is 7 chars

constexpr int SEPARATOR2_Y = KV_FIRST_Y + 5 * KV_STRIDE + 2;    // 118

// Body block: from below sep2 down to footer.
constexpr int BODY_FIRST_Y = SEPARATOR2_Y + 4;                  // 122
constexpr int BODY_STRIDE  = FONT_H;                            // 16

constexpr int FOOTER_Y     = SCREEN_H - FONT_H - 6;             // 218
constexpr int SEPARATOR3_Y = FOOTER_Y - 4;                      // 214

// Max body lines: (SEPARATOR3_Y - BODY_FIRST_Y) / BODY_STRIDE
//   = (214 - 122) / 16 = 5 lines × 38 chars = 190 chars,
// which exceeds the 100-char body bound — fits any entry.
constexpr int BODY_MAX_LINES = (SEPARATOR3_Y - BODY_FIRST_Y) / BODY_STRIDE;
constexpr int BODY_WRAP_COLS = (SCREEN_W - 2 * PAD_X) / FONT_W; // 39, use 38 for safety

// ── Colors ───────────────────────────────────────────────────────────
constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_FG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_TYPE_UNREAD = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_TYPE_READ   = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_TYPE_STORE  = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_TYPE_DELIV  = NANOJS8_COLOR_GRAY;
// L7.11g.7: distinct color/label for STORE rows we're holding on
// behalf of a third party (received via "MSG TO:<recipient>" from
// another operator). Magenta matches screen_inbox.cpp's row palette.
constexpr uint16_t COL_TYPE_RELAY  = NANOJS8_COLOR_MAGENTA;
constexpr uint16_t COL_KV_LABEL    = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_KV_VALUE    = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_BODY        = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_STALE       = NANOJS8_COLOR_RED;

// ── State (BSS) ──────────────────────────────────────────────────────
nanojs8_mailbox_entry_t s_entry;
bool                    s_entry_valid = false;     // true after a successful capture in on_enter

// Dirty-tracking: render() compares against last paint to avoid
// redundant SPI traffic. The only dynamic field in steady state is
// the type (if a background task marks delivered/etc).
uint16_t s_last_id   = 0;
uint8_t  s_last_type = 0xFF;

// ── Helpers ──────────────────────────────────────────────────────────

const char *type_label(uint8_t t) {
    switch (t) {
        case NANOJS8_MAILBOX_TYPE_UNREAD:    return "UNREAD";
        case NANOJS8_MAILBOX_TYPE_READ:      return "READ";
        case NANOJS8_MAILBOX_TYPE_STORE:     return "STORE";
        case NANOJS8_MAILBOX_TYPE_DELIVERED: return "DELIVERED";
        case NANOJS8_MAILBOX_TYPE_EMPTY:     return "EMPTY";
        default:                              return "?";
    }
}

uint16_t color_for_type(uint8_t t) {
    switch (t) {
        case NANOJS8_MAILBOX_TYPE_UNREAD:    return COL_TYPE_UNREAD;
        case NANOJS8_MAILBOX_TYPE_READ:      return COL_TYPE_READ;
        case NANOJS8_MAILBOX_TYPE_STORE:     return COL_TYPE_STORE;
        case NANOJS8_MAILBOX_TYPE_DELIVERED: return COL_TYPE_DELIV;
        default:                              return COL_TYPE_READ;
    }
}

// L7.11g.7: is this a relay we're holding for a third party? STORE
// row whose from_call differs from our configured callsign. Case-
// insensitive. Returns false if our callsign isn't set (won't
// mis-flag anything).
bool is_remote_store(const nanojs8_mailbox_entry_t *e) {
    if (!e || e->type != NANOJS8_MAILBOX_TYPE_STORE) return false;
    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (!cfg || cfg->callsign[0] == '\0') return false;
    return strcasecmp(e->from_call, cfg->callsign) != 0;
}

// Header label: "STORE" for locally-composed holds, "RELAY" for
// remote-store holds. Other types use the standard type_label.
const char *type_label_for_entry(const nanojs8_mailbox_entry_t *e) {
    if (is_remote_store(e)) return "RELAY";
    return type_label(e->type);
}

// Header tag color: matches the entry-aware label.
uint16_t color_for_entry(const nanojs8_mailbox_entry_t *e) {
    if (is_remote_store(e)) return COL_TYPE_RELAY;
    return color_for_type(e->type);
}

// Word-wrap body into up to BODY_MAX_LINES lines, each up to 38 chars.
// Newlines in the source force a break. Returns number of lines used.
// Output buffer: lines[BODY_MAX_LINES][BODY_WRAP_COLS+1].
int wrap_body(const char *src, char lines[][BODY_WRAP_COLS + 1]) {
    int line_idx = 0;
    int col_idx  = 0;
    int last_space_col = -1;   // column index of last space in current line
    const int max_cols = BODY_WRAP_COLS - 1;  // leave 1 char headroom

    lines[0][0] = '\0';

    for (size_t i = 0; src && src[i] != '\0'; ++i) {
        const char c = src[i];
        if (line_idx >= BODY_MAX_LINES) break;

        if (c == '\n' || c == '\r') {
            // Hard break.
            lines[line_idx][col_idx] = '\0';
            ++line_idx;
            if (line_idx < BODY_MAX_LINES) lines[line_idx][0] = '\0';
            col_idx = 0;
            last_space_col = -1;
            continue;
        }

        const char ch = (c == '\t') ? ' ' : c;

        if (col_idx >= max_cols) {
            // Soft-wrap point: break at the last space if any, else hard.
            if (last_space_col > 0) {
                // Carry the partial word to the next line.
                const int carry_start = last_space_col + 1;
                const int carry_len   = col_idx - carry_start;
                char carry[BODY_WRAP_COLS + 1];
                memcpy(carry, &lines[line_idx][carry_start], carry_len);
                carry[carry_len] = '\0';
                lines[line_idx][last_space_col] = '\0';
                ++line_idx;
                if (line_idx >= BODY_MAX_LINES) break;
                memcpy(lines[line_idx], carry, carry_len + 1);
                col_idx = carry_len;
                last_space_col = -1;
            } else {
                lines[line_idx][col_idx] = '\0';
                ++line_idx;
                if (line_idx >= BODY_MAX_LINES) break;
                lines[line_idx][0] = '\0';
                col_idx = 0;
                last_space_col = -1;
            }
        }

        if (col_idx < max_cols) {
            if (ch == ' ') last_space_col = col_idx;
            lines[line_idx][col_idx++] = ch;
            lines[line_idx][col_idx]   = '\0';
        }
    }

    if (col_idx > 0) ++line_idx;
    return line_idx;
}

// ── Painting ─────────────────────────────────────────────────────────

void paint_header_chrome(const nanojs8_mailbox_entry_t *e) {
    nanojs8_display_fill_rect(0, 0, SCREEN_W, SEPARATOR1_Y + 1, COL_BG);

    char head_buf[16];
    snprintf(head_buf, sizeof(head_buf), "MSG #%u", (unsigned)e->id);
    nanojs8_display_draw_text(PAD_X, HEADER_Y, head_buf,
                                COL_HEADER_NAME, COL_BG);

    // L7.11g.7: entry-aware label/color so remote-store rows show
    // "RELAY" in magenta instead of "STORE" in yellow.
    const char    *tag  = type_label_for_entry(e);
    const uint16_t col  = color_for_entry(e);
    const int      w    = nanojs8_display_text_width(tag);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - w, HEADER_Y, tag,
                                col, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR1_Y, SCREEN_W, 1, COL_SEPARATOR);
}

void paint_kv_row(int row, const char *label, const char *value,
                   uint16_t value_col) {
    const int y = KV_FIRST_Y + row * KV_STRIDE;
    nanojs8_display_fill_rect(0, y, SCREEN_W, FONT_H, COL_BG);
    nanojs8_display_draw_text(KV_LABEL_X, y, label, COL_KV_LABEL, COL_BG);
    nanojs8_display_draw_text(KV_VALUE_X, y, value, value_col, COL_BG);
}

void paint_kv_block() {
    // From
    paint_kv_row(0, "From:", s_entry.from_call, COL_KV_VALUE);
    // To
    paint_kv_row(1, "To:",   s_entry.to_call,   COL_KV_VALUE);
    // At
    char at_buf[16];
    const uint32_t s = s_entry.utc_seconds_today % 86400u;
    snprintf(at_buf, sizeof(at_buf),
             "%02u:%02u:%02u UTC",
             (unsigned)(s / 3600u),
             (unsigned)((s % 3600u) / 60u),
             (unsigned)(s % 60u));
    paint_kv_row(2, "At:", at_buf, COL_KV_VALUE);
    // SNR — meaningful only for inbound types
    char snr_buf[16];
    if (s_entry.type == NANOJS8_MAILBOX_TYPE_UNREAD ||
        s_entry.type == NANOJS8_MAILBOX_TYPE_READ) {
        snprintf(snr_buf, sizeof(snr_buf), "%+d dB", (int)s_entry.snr_db);
    } else {
        snprintf(snr_buf, sizeof(snr_buf), "n/a");
    }
    paint_kv_row(3, "SNR:", snr_buf, COL_KV_VALUE);
    // Freq — meaningful only for inbound types
    char freq_buf[16];
    if (s_entry.freq_hz > 0) {
        snprintf(freq_buf, sizeof(freq_buf), "%u Hz",
                 (unsigned)s_entry.freq_hz);
    } else {
        snprintf(freq_buf, sizeof(freq_buf), "n/a");
    }
    paint_kv_row(4, "Freq:", freq_buf, COL_KV_VALUE);

    nanojs8_display_fill_rect(0, SEPARATOR2_Y, SCREEN_W, 1, COL_SEPARATOR);
}

void paint_body() {
    // Clear the body region.
    nanojs8_display_fill_rect(0, SEPARATOR2_Y + 1,
                                SCREEN_W,
                                SEPARATOR3_Y - (SEPARATOR2_Y + 1),
                                COL_BG);

    char lines[BODY_MAX_LINES][BODY_WRAP_COLS + 1];
    for (int i = 0; i < BODY_MAX_LINES; ++i) lines[i][0] = '\0';
    const int n = wrap_body(s_entry.body, lines);

    for (int i = 0; i < n && i < BODY_MAX_LINES; ++i) {
        const int y = BODY_FIRST_Y + i * BODY_STRIDE;
        nanojs8_display_draw_text(PAD_X, y, lines[i], COL_BODY, COL_BG);
    }
}

void paint_footer() {
    nanojs8_display_fill_rect(0, SEPARATOR3_Y, SCREEN_W, 1, COL_SEPARATOR);
    nanojs8_display_fill_rect(0, FOOTER_Y, SCREEN_W, FONT_H, COL_BG);
    nanojs8_display_draw_text(PAD_X, FOOTER_Y,
                                "BACKSPACE return",
                                COL_FOOTER, COL_BG);
}

void paint_stale_notice() {
    nanojs8_display_fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);

    const char *m1 = "Entry no longer available.";
    const char *m2 = "It may have been evicted or";
    const char *m3 = "deleted by another action.";
    const int y1 = 80;
    const int y2 = y1 + FONT_H + 4;
    const int y3 = y2 + FONT_H + 4;
    int w;
    w = nanojs8_display_text_width(m1);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, y1, m1, COL_STALE, COL_BG);
    w = nanojs8_display_text_width(m2);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, y2, m2, COL_FOOTER, COL_BG);
    w = nanojs8_display_text_width(m3);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, y3, m3, COL_FOOTER, COL_BG);

    nanojs8_display_draw_text(PAD_X, FOOTER_Y,
                                "BACKSPACE return",
                                COL_FOOTER, COL_BG);
}

// ── Render entry points ──────────────────────────────────────────────

void render(bool full_redraw) {
    // Re-fetch the entry every render to catch external state changes
    // (e.g. a background task marks delivered). Cheap — find_by_id is
    // a 16-slot linear scan under the mailbox mutex.
    if (s_entry_valid) {
        nanojs8_mailbox_entry_t fresh;
        if (nanojs8_mailbox_find_by_id(s_entry.id, &fresh)) {
            s_entry = fresh;
        } else {
            s_entry_valid = false;
        }
    }

    if (!s_entry_valid) {
        if (full_redraw || s_last_type != 0xFE) {
            paint_stale_notice();
            s_last_id   = 0;
            s_last_type = 0xFE;     // sentinel meaning "stale painted"
        }
        return;
    }

    const bool changed = full_redraw ||
                         s_last_id   != s_entry.id ||
                         s_last_type != s_entry.type;
    if (!changed) return;

    // L7.11g.3-fix2: full-screen clear on first paint after on_enter.
    // The paint_*() functions below only clear their own strips
    // (header band, each kv row's 16-px text strip, the body region,
    // the footer). Between kv rows there are 2-px gaps not covered
    // by paint_kv_row, and 3-4 px gaps either side of the separators
    // that paint_kv_block/paint_body don't touch. Those gaps show
    // whatever was painted previously — when arriving from INBOX,
    // that's INBOX's row text and dividers. Pre-clearing here
    // guarantees a clean canvas before the static structure repaints.
    // Matches the convention used by screen_inbox.cpp render().
    if (full_redraw) {
        nanojs8_display_fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    }

    // Full repaint of the static structure on any change. Cheaper than
    // tracking each row's dirtiness individually; this screen is
    // rarely entered relative to INBOX list scrolling.
    paint_header_chrome(&s_entry);
    paint_kv_block();
    paint_body();
    paint_footer();

    s_last_id   = s_entry.id;
    s_last_type = s_entry.type;
}

void on_enter() {
    const uint16_t want = nanojs8_screen_inbox_selected_id();
    if (want == 0) {
        ESP_LOGW(TAG, "Entered with no selected id — showing stale notice");
        s_entry_valid = false;
        s_last_type   = 0xFD;    // force repaint to stale-notice path
        return;
    }
    if (nanojs8_mailbox_find_by_id(want, &s_entry)) {
        s_entry_valid = true;
        s_last_id   = 0;          // force full repaint
        s_last_type = 0xFC;
        ESP_LOGI(TAG, "Entering INBOX_DETAIL for id=%u (type=%s)",
                 (unsigned)want, type_label(s_entry.type));
    } else {
        ESP_LOGW(TAG, "find_by_id(%u) failed — entry gone", (unsigned)want);
        s_entry_valid = false;
        s_last_type   = 0xFD;
    }
}

bool handle_input(uint8_t event) {
    // BACKSPACE (0x08) or TRACKBALL_LEFT → return to INBOX list.
    if (event == 0x08 || event == NANOJS8_TRACKBALL_LEFT) {
        nanojs8_ui_set_screen(NANOJS8_SCREEN_INBOX);
        return true;
    }
    // No other actions on this screen — body is read-only, type
    // transitions happen from INBOX.
    return false;
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_INBOX_DETAIL = {
    .id           = NANOJS8_SCREEN_INBOX_DETAIL,
    .name         = "INBOX_DETAIL",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
