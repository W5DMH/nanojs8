/*
 * screen_directed.cpp — L7.10 DIRECTED traffic screen (filtered)
 * ===============================================================
 * Like the ALL screen, but only entries addressed personally to us
 * or to a group we belong to. Filter (matches MicroJS8 app.py ~2017
 * minus the @ALLCALL branch, per operator preference for L7.10):
 *
 *   to_call (case-insensitive) == my callsign        → keep
 *   to_call is @<group>, group is in my groups list  → keep
 *   to_call is @ALLCALL or @HB                       → drop
 *   to_call empty (heartbeat)                        → drop
 *
 * Bodies wrap onto continuation rows; screen-switch is tick-debounced
 * so a normal trackball roll doesn't blow past the screen.
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "activity.h"
#include "config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "screen_dir";

// ── Layout (mirrors ALL screen) ──────────────────────────────────────
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
constexpr int CONTENT_H        = FOOTER_Y - FIRST_ROW_Y - 4;
constexpr int MAX_VISIBLE_ROWS = CONTENT_H / ROW_STRIDE;

constexpr int LINE_X       = PAD_X;
// L7.10-fix2: timestamp dropped; 3-char wrap indent under direction marker.
constexpr int BODY_CONT_X  = PAD_X + 3 * FONT_W;
constexpr int MAX_ROWS_PER_ENTRY = 3;

constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_FG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_HINT        = NANOJS8_COLOR_DARK_GRAY;

// L7.10-fix2: row freshness thresholds — match HEARD and ALL.
constexpr uint32_t AGE_FRESH_S  = 30u * 60u;
constexpr uint32_t AGE_RECENT_S = 4u * 60u * 60u;
constexpr uint16_t COL_AGE_FRESH  = NANOJS8_COLOR_GREEN;
constexpr uint16_t COL_AGE_RECENT = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_AGE_OLD    = NANOJS8_COLOR_GRAY;

constexpr int SCROLL_TICK_THRESHOLD = 4;
constexpr int SWITCH_TICK_THRESHOLD = 4;

// ── State (BSS) ──────────────────────────────────────────────────────
int s_scroll_offset = 0;
int s_up_ticks    = 0;
int s_down_ticks  = 0;
int s_left_ticks  = 0;
int s_right_ticks = 0;

// Raw activity snapshot (unfiltered, ~6 KB).
nanojs8_activity_directed_t s_snapshot[NANOJS8_ACTIVITY_DIRECTED_MAX];
// Filtered view — pointers into s_snapshot; same lifetime, no copy.
const nanojs8_activity_directed_t *s_filtered[NANOJS8_ACTIVITY_DIRECTED_MAX];
uint32_t s_filtered_count = 0;

// Dirty-detection.
// L7.10-fix2: minute is tracked so freshness colors refresh at the 30-min
// and 4-h boundaries even on quiet bands.
uint32_t s_last_count       = 0xFFFFFFFFu;
uint32_t s_last_newest_at_s = 0;
int      s_last_scroll      = -1;
uint32_t s_last_minute      = 0xFFFFFFFFu;

// ── Helpers ──────────────────────────────────────────────────────────

uint16_t color_for_age(uint32_t age_s) {
    if (age_s < AGE_FRESH_S)  return COL_AGE_FRESH;
    if (age_s < AGE_RECENT_S) return COL_AGE_RECENT;
    return COL_AGE_OLD;
}

void paint_chrome() {
    nanojs8_display_clear(COL_BG);

    nanojs8_display_draw_text(PAD_X, HEADER_Y, "NanoJS8",
                              COL_HEADER_NAME, COL_BG);
    const char *tag = "DIRECTED";
    int tag_w = nanojs8_display_text_width(tag);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - tag_w, HEADER_Y, tag,
                              COL_HEADER_FG, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
}

int paint_entry(int y, const nanojs8_activity_directed_t &d, int max_rows_avail,
                uint32_t now_b)
{
    if (max_rows_avail <= 0) return 0;

    const bool inbound = (d.direction == NANOJS8_ACTIVITY_DIR_IN);

    const uint32_t age_s =
        (now_b > d.at_boot_s) ? (now_b - d.at_boot_s) : 0;
    // L7.11f-fix2c: outbound entries (our own transmits from COMPOSE +
    // ALLCALL) render in red, regardless of age. The freshness palette
    // (green / yellow / gray) only applies to inbound. Whole-row red is
    // intentionally more distinct than MicroJS8's mixed-colour scheme:
    // operators glancing at the screen instantly see which messages
    // are theirs vs which were received.
    const uint16_t row_color = inbound ? color_for_age(age_s)
                                        : NANOJS8_COLOR_RED;

    nanojs8_display_fill_rect(0, y, SCREEN_W, ROW_STRIDE, COL_BG);

    int x = LINE_X;

    x += nanojs8_display_draw_text(x, y, inbound ? ">" : "<",
                                    row_color, COL_BG);
    x += FONT_W;

    x += nanojs8_display_draw_text(x, y, d.from_call, row_color, COL_BG);

    if (d.to_call[0]) {
        x += nanojs8_display_draw_text(x, y, ">", row_color, COL_BG);
        x += nanojs8_display_draw_text(x, y, d.to_call, row_color, COL_BG);
    }
    x += FONT_W;

    x += nanojs8_display_draw_text(x, y, d.verb, row_color, COL_BG);

    int rows_used = 1;
    if (d.body[0]) {
        x += FONT_W;

        const int body_len   = (int)strnlen(d.body, NANOJS8_ACTIVITY_BODY_MAX);
        const char *body_ptr = d.body;
        int row_x_avail      = SCREEN_W - x;
        int chars_fit_row1   = row_x_avail / FONT_W;
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
    const char *l1 = "no traffic for you yet";
    const char *l2 = "messages to your call or groups will appear here";
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
             "%u for you   UP/DN scroll   L/R switch",
             (unsigned)total);
    nanojs8_display_draw_text(PAD_X, FOOTER_Y, buf, COL_FOOTER, COL_BG);
}

/// Rebuild s_filtered[] by passing s_snapshot[] through
/// nanojs8_activity_is_for_me(). Filter pulls operator's callsign +
/// groups from the live config cache on every call (no caching to
/// avoid staleness if the operator edits SETUP).
///
/// L7.11f-fix2d: outbound (DIR_OUT) entries are ALWAYS accepted, no
/// matter their to_call. The is_for_me() helper treats @HB/@ALLCALL
/// as broadcasts (returns false) — which is the right policy for
/// inbound traffic (someone else's heartbeat isn't "for us"), but
/// wrong for outbound (our OWN heartbeat absolutely is the operator's
/// own traffic and the DIRECTED screen is where it belongs in red).
/// is_for_me() now only gates inbound entries.
void rebuild_filter(uint32_t snapshot_n) {
    s_filtered_count = 0;
    const nanojs8_config_t *cfg = nanojs8_config_get();
    const char *my_call    = cfg ? cfg->callsign : "";
    const char *my_groups  = cfg ? cfg->groups   : "";
    for (uint32_t i = 0; i < snapshot_n; ++i) {
        const bool is_out =
            (s_snapshot[i].direction == NANOJS8_ACTIVITY_DIR_OUT);
        const bool keep =
            is_out ||
            nanojs8_activity_is_for_me(s_snapshot[i].to_call,
                                        my_call, my_groups);
        if (keep) {
            s_filtered[s_filtered_count++] = &s_snapshot[i];
        }
    }
}

void on_enter() {
    ESP_LOGI(TAG, "Entering DIRECTED");
    s_scroll_offset    = 0;
    s_up_ticks = s_down_ticks = s_left_ticks = s_right_ticks = 0;
    s_last_count       = 0xFFFFFFFFu;
    s_last_newest_at_s = 0;
    s_last_scroll      = -1;
    s_last_minute      = 0xFFFFFFFFu;
    paint_chrome();
}

void render(bool /*full_redraw*/) {
    const uint32_t n_raw = nanojs8_activity_snapshot_directed(
        s_snapshot, sizeof(s_snapshot) / sizeof(s_snapshot[0]));
    rebuild_filter(n_raw);

    const uint32_t n           = s_filtered_count;
    const uint32_t newest_at_s = (n > 0) ? s_filtered[0]->at_boot_s : 0;
    // L7.10-fix2: shared timer read drives both the minute-boundary
    // dirty-check and the per-entry age math.
    const uint32_t now_b      = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint32_t cur_minute = now_b / 60u;

    if (n == s_last_count &&
        newest_at_s == s_last_newest_at_s &&
        s_scroll_offset == s_last_scroll &&
        cur_minute == s_last_minute) {
        return;
    }

    int max_off = (int)n - 1;
    if (max_off < 0) max_off = 0;
    if (s_scroll_offset > max_off) s_scroll_offset = max_off;
    if (s_scroll_offset < 0)        s_scroll_offset = 0;

    int y         = FIRST_ROW_Y;
    int entry_idx = s_scroll_offset;
    int rows_left = MAX_VISIBLE_ROWS;

    while (rows_left > 0 && entry_idx < (int)n) {
        const int rows = paint_entry(y, *s_filtered[entry_idx],
                                      rows_left, now_b);
        if (rows <= 0) break;
        y         += rows * ROW_STRIDE;
        rows_left -= rows;
        ++entry_idx;
    }

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
    // LEFT → ALL, RIGHT → HOME (closes the cycle).
    if (event == NANOJS8_TRACKBALL_LEFT) {
        s_right_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_ALL);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        s_left_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            // L7.11g.3: INBOX inserted between DIRECTED and COMPOSE.
            // Ring is now HEARD → ALL → DIRECTED → INBOX → COMPOSE →
            // ALLCALL. Reading screens come first, then mailbox, then
            // the TX cluster (COMPOSE + ALLCALL).
            nanojs8_ui_set_screen(NANOJS8_SCREEN_INBOX);
        }
        return true;
    }
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

extern const nanojs8_screen_t SCREEN_DIRECTED = {
    .id           = NANOJS8_SCREEN_DIRECTED,
    .name         = "DIRECTED",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
