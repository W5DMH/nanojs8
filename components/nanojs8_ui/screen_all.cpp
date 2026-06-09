/*
 * screen_all.cpp — L7.10 ALL traffic screen
 * ==========================================
 * Chat-style log of EVERY protocol-verb exchange we've decoded.
 *
 *   > HH:MM:SS  FROM     VERB body         (heartbeat — no TO)
 *   > HH:MM:SS  FROM>TO  VERB body         (directed)
 *
 * Newest at top. Long bodies wrap onto continuation rows indented
 * under the verb. Scrollable with trackball UP/DOWN. Screen-switch
 * via LEFT/RIGHT, debounced to a 4-pulse tick threshold so that a
 * single trackball roll doesn't cycle through five screens.
 *
 * This is the wide-firehose view. DIRECTED is the filtered subset
 * for things addressed to *us* specifically (our callsign + groups).
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "activity.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "screen_all";

// ── Layout ───────────────────────────────────────────────────────────
constexpr int SCREEN_W   = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H   = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H     = 16;
constexpr int FONT_W     = 8;
constexpr int PAD_X      = 4;

constexpr int HEADER_Y    = 6;
constexpr int SEPARATOR_Y = HEADER_Y + FONT_H + 4;

constexpr int FIRST_ROW_Y = SEPARATOR_Y + 6;
constexpr int ROW_STRIDE  = FONT_H;

constexpr int FOOTER_Y    = SCREEN_H - FONT_H - 6;

// Total vertical span available for entries. Wrap math budgets against this.
constexpr int CONTENT_H        = FOOTER_Y - FIRST_ROW_Y - 4;
constexpr int MAX_VISIBLE_ROWS = CONTENT_H / ROW_STRIDE;

constexpr int LINE_X       = PAD_X;
// L7.10-fix2: timestamp dropped from row; continuation indent shrinks
// to 3 chars past the direction marker so wrapped body lines still read
// as clearly subordinate to the entry header.
constexpr int BODY_CONT_X  = PAD_X + 3 * FONT_W;     // 28 px
// Each entry is allowed at most this many wrapped rows. Beyond that the
// final visible row ends in '…' (using ASCII '.' x3) so the operator
// knows the message continues.
constexpr int MAX_ROWS_PER_ENTRY = 3;

constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_FG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_HINT        = NANOJS8_COLOR_DARK_GRAY;

// L7.10-fix2: row freshness — mirrors HEARD's thresholds so the three
// list screens have a single shared visual language. Direction is now
// conveyed by the '>' / '<' glyph alone; color carries the age signal.
constexpr uint32_t AGE_FRESH_S  = 30u * 60u;        // 30 minutes → green
constexpr uint32_t AGE_RECENT_S = 4u * 60u * 60u;   // 4 hours    → yellow
constexpr uint16_t COL_AGE_FRESH  = NANOJS8_COLOR_GREEN;
constexpr uint16_t COL_AGE_RECENT = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_AGE_OLD    = NANOJS8_COLOR_GRAY;

// L7.10-fix2: direction colors retired. The '>' / '<' glyph alone
// signals direction now; row text color encodes age via color_for_age().

// Trackball tick thresholds — 4 pulses per scroll step or screen-switch.
// Matches SETUP's FOCUS_TICK_THRESHOLD. Without this, the C3 keyboard's
// 50 ms trackball poll rate cycles screens almost instantaneously when
// the operator gives the ball a normal "one-direction roll".
constexpr int SCROLL_TICK_THRESHOLD = 4;
constexpr int SWITCH_TICK_THRESHOLD = 4;

// ── State (all in BSS, never on stack) ───────────────────────────────
int s_scroll_offset = 0;
int s_up_ticks    = 0;
int s_down_ticks  = 0;
int s_left_ticks  = 0;
int s_right_ticks = 0;

// Snapshot buffer for render — static-BSS, ~6 KB, not stack.
nanojs8_activity_directed_t s_render_rows[NANOJS8_ACTIVITY_DIRECTED_MAX];

// Dirty-detection: skip render entirely if nothing the operator can see
// has changed. Tracks (count, newest at_boot_s, scroll_offset, minute).
// L7.10-fix2: minute is included so freshness colors get re-evaluated
// at the 30-min and 4-h boundaries even on a quiet band where no new
// data arrives to naturally trigger a repaint.
uint32_t s_last_count       = 0xFFFFFFFFu;
uint32_t s_last_newest_at_s = 0;
int      s_last_scroll      = -1;
uint32_t s_last_minute      = 0xFFFFFFFFu;

// ── Helpers ──────────────────────────────────────────────────────────

/// L7.10-fix2: row freshness mapping — mirrors HEARD screen exactly so
/// the operator's visual mental model is shared across all three list
/// screens. age_s is computed once per entry in paint_entry().
uint16_t color_for_age(uint32_t age_s) {
    if (age_s < AGE_FRESH_S)  return COL_AGE_FRESH;
    if (age_s < AGE_RECENT_S) return COL_AGE_RECENT;
    return COL_AGE_OLD;
}

void paint_chrome() {
    nanojs8_display_clear(COL_BG);

    nanojs8_display_draw_text(PAD_X, HEADER_Y, "NanoJS8",
                              COL_HEADER_NAME, COL_BG);
    const char *tag = "ALL";
    int tag_w = nanojs8_display_text_width(tag);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - tag_w, HEADER_Y, tag,
                              COL_HEADER_FG, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
}

/// Paint one DIRECTED row at y, possibly wrapping body onto continuation
/// rows below. Returns the number of vertical rows consumed (1..MAX_ROWS_PER_ENTRY).
///
/// L7.10-fix2: row layout dropped the explicit HH:MM:SS column. Freshness
/// is now encoded by color (green ≤30 min, yellow 30 min – 4 h, gray >4 h),
/// matching the HEARD screen's visual idiom. The '>' / '<' glyph alone
/// carries direction.
int paint_entry(int y, const nanojs8_activity_directed_t &d, int max_rows_avail,
                uint32_t now_b)
{
    if (max_rows_avail <= 0) return 0;

    const bool inbound = (d.direction == NANOJS8_ACTIVITY_DIR_IN);

    // Age → row color. Inbound and outbound share the same mapping.
    const uint32_t age_s =
        (now_b > d.at_boot_s) ? (now_b - d.at_boot_s) : 0;
    const uint16_t row_color = color_for_age(age_s);

    // Erase first row band.
    nanojs8_display_fill_rect(0, y, SCREEN_W, ROW_STRIDE, COL_BG);

    int x = LINE_X;

    // Direction glyph — same freshness color as the rest of the row so
    // the entry reads as one unit. Direction is in the glyph itself.
    x += nanojs8_display_draw_text(x, y, inbound ? ">" : "<",
                                    row_color, COL_BG);
    x += FONT_W;  // gap

    // FROM call.
    x += nanojs8_display_draw_text(x, y, d.from_call, row_color, COL_BG);

    // Optional ">TO". The internal '>' separator stays the same color as
    // the surrounding callsigns — it's part of the addressing pair, not
    // a direction marker.
    if (d.to_call[0]) {
        x += nanojs8_display_draw_text(x, y, ">", row_color, COL_BG);
        x += nanojs8_display_draw_text(x, y, d.to_call, row_color, COL_BG);
    }
    x += FONT_W;

    // VERB.
    x += nanojs8_display_draw_text(x, y, d.verb, row_color, COL_BG);

    // Body wrap. Same row_color throughout — SNR-sign accent coloring
    // from L7.9 is retired so freshness reads cleanly. The +/-NN value
    // is still legible from the digits themselves.
    int rows_used = 1;

    if (d.body[0]) {
        x += FONT_W;  // gap before body

        const int body_len     = (int)strnlen(d.body, NANOJS8_ACTIVITY_BODY_MAX);
        const char *body_ptr   = d.body;
        int   row_x_avail      = SCREEN_W - x;
        int   chars_fit_row1   = row_x_avail / FONT_W;
        if (chars_fit_row1 < 0) chars_fit_row1 = 0;
        if (chars_fit_row1 > body_len) chars_fit_row1 = body_len;

        if (chars_fit_row1 > 0) {
            char chunk[NANOJS8_ACTIVITY_BODY_MAX + 1];
            const int n = chars_fit_row1;
            memcpy(chunk, body_ptr, n);
            chunk[n] = '\0';
            nanojs8_display_draw_text(x, y, chunk, row_color, COL_BG);
            body_ptr += n;
        }

        int remaining = body_len - chars_fit_row1;
        int chars_per_cont_row = (SCREEN_W - BODY_CONT_X) / FONT_W;
        if (chars_per_cont_row < 1) chars_per_cont_row = 1;

        while (remaining > 0 && rows_used < MAX_ROWS_PER_ENTRY
               && rows_used < max_rows_avail) {
            const int y2 = y + rows_used * ROW_STRIDE;
            nanojs8_display_fill_rect(0, y2, SCREEN_W, ROW_STRIDE, COL_BG);

            int n = (remaining < chars_per_cont_row) ? remaining
                                                     : chars_per_cont_row;
            char chunk[NANOJS8_ACTIVITY_BODY_MAX + 1];
            const bool will_truncate =
                (remaining > n) && (rows_used + 1 >= MAX_ROWS_PER_ENTRY ||
                                    rows_used + 1 >= max_rows_avail);
            if (will_truncate && n > 3) {
                memcpy(chunk, body_ptr, (size_t)(n - 3));
                chunk[n - 3] = '.';
                chunk[n - 2] = '.';
                chunk[n - 1] = '.';
                chunk[n]     = '\0';
                remaining = 0;
            } else {
                memcpy(chunk, body_ptr, (size_t)n);
                chunk[n] = '\0';
                body_ptr  += n;
                remaining -= n;
            }
            nanojs8_display_draw_text(BODY_CONT_X, y2, chunk,
                                      row_color, COL_BG);
            ++rows_used;
        }
    }

    return rows_used;
}

void paint_empty_state() {
    const char *l1 = "no traffic decoded yet";
    const char *l2 = "tune to 7.078 MHz USB and wait";
    int w1 = nanojs8_display_text_width(l1);
    int w2 = nanojs8_display_text_width(l2);
    int y  = FIRST_ROW_Y + CONTENT_H / 2 - FONT_H;
    nanojs8_display_draw_text((SCREEN_W - w1) / 2, y, l1,
                              COL_HINT, COL_BG);
    nanojs8_display_draw_text((SCREEN_W - w2) / 2, y + FONT_H + 2, l2,
                              COL_HINT, COL_BG);
}

void paint_footer(uint32_t total) {
    nanojs8_display_fill_rect(0, FOOTER_Y - 2, SCREEN_W, FONT_H + 4, COL_BG);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%u total   UP/DN scroll   L/R switch",
             (unsigned)total);
    nanojs8_display_draw_text(PAD_X, FOOTER_Y, buf, COL_FOOTER, COL_BG);
}

void on_enter() {
    ESP_LOGI(TAG, "Entering ALL");
    s_scroll_offset    = 0;
    s_up_ticks = s_down_ticks = s_left_ticks = s_right_ticks = 0;
    // Force a full repaint on the first render after entry.
    s_last_count       = 0xFFFFFFFFu;
    s_last_newest_at_s = 0;
    s_last_scroll      = -1;
    s_last_minute      = 0xFFFFFFFFu;
    paint_chrome();
}

void render(bool /*full_redraw*/) {
    const uint32_t n_raw = nanojs8_activity_snapshot_directed(
        s_render_rows, sizeof(s_render_rows) / sizeof(s_render_rows[0]));

    // L7.11f-fix2d: drop OUT entries from the ALL firehose. Operator's
    // own transmissions live on DIRECTED (in red) and don't belong in
    // the inbound-traffic survey ALL provides. Compact in place — the
    // snapshot is already sorted newest-first and we want to preserve
    // that order; this is a stable filter, O(n), no extra storage.
    uint32_t n = 0;
    for (uint32_t i = 0; i < n_raw; ++i) {
        if (s_render_rows[i].direction == NANOJS8_ACTIVITY_DIR_OUT) {
            continue;
        }
        if (n != i) {
            s_render_rows[n] = s_render_rows[i];
        }
        ++n;
    }

    // Dirty detection: skip work entirely if neither the data nor the
    // operator's scroll position has changed AND we haven't crossed a
    // minute boundary (which could shift freshness coloring).
    const uint32_t newest_at_s = (n > 0) ? s_render_rows[0].at_boot_s : 0;
    // L7.10-fix2: single timer read used for BOTH the freshness-minute
    // dirty check and the per-entry age computation. Cheaper and keeps
    // the two age signals (color bucket, repaint trigger) in lockstep.
    const uint32_t now_b      = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint32_t cur_minute = now_b / 60u;
    if (n == s_last_count &&
        newest_at_s == s_last_newest_at_s &&
        s_scroll_offset == s_last_scroll &&
        cur_minute == s_last_minute) {
        return;
    }

    // Clamp scroll within bounds (n may have shrunk if entries aged out).
    int max_off = (int)n - 1;
    if (max_off < 0) max_off = 0;
    if (s_scroll_offset > max_off) s_scroll_offset = max_off;
    if (s_scroll_offset < 0)        s_scroll_offset = 0;

    // Walk entries top-down, allocating rows by what each one consumes.
    int y         = FIRST_ROW_Y;
    int entry_idx = s_scroll_offset;
    int rows_left = MAX_VISIBLE_ROWS;

    while (rows_left > 0 && entry_idx < (int)n) {
        const int rows = paint_entry(y, s_render_rows[entry_idx],
                                      rows_left, now_b);
        if (rows <= 0) break;
        y         += rows * ROW_STRIDE;
        rows_left -= rows;
        ++entry_idx;
    }

    // Clear unused trailing space.
    if (y < FOOTER_Y - 2) {
        nanojs8_display_fill_rect(0, y, SCREEN_W, FOOTER_Y - 2 - y, COL_BG);
    }

    if (n == 0) paint_empty_state();
    paint_footer(n);

    s_last_count       = n;
    s_last_newest_at_s = newest_at_s;
    s_last_scroll      = s_scroll_offset;
    s_last_minute      = cur_minute;
}

bool handle_input(uint8_t event) {
    // Left/Right with tick threshold — debounces against the C3
    // keyboard's 50 ms poll rate so that a normal trackball flick
    // doesn't cycle through every screen.
    if (event == NANOJS8_TRACKBALL_LEFT) {
        s_right_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_HEARD);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        s_left_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_DIRECTED);
        }
        return true;
    }
    // Up/Down scroll with existing 4-tick pacing.
    if (event == NANOJS8_TRACKBALL_UP) {
        s_down_ticks = 0;
        if (++s_up_ticks >= SCROLL_TICK_THRESHOLD) {
            s_up_ticks = 0;
            if (s_scroll_offset > 0) s_scroll_offset--;
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_DOWN) {
        s_up_ticks = 0;
        if (++s_down_ticks >= SCROLL_TICK_THRESHOLD) {
            s_down_ticks = 0;
            ++s_scroll_offset;
        }
        return true;
    }
    return false;
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_ALL = {
    .id           = NANOJS8_SCREEN_ALL,
    .name         = "ALL",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
