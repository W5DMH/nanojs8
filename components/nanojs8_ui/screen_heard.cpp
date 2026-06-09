/*
 * screen_heard.cpp — L7.9 HEARD screen (L7.13: real SNR)
 * =====================================
 * Lists every callsign we've decoded recently. Columns mirror MicroJS8:
 *   CALL · SNR · GRID · MI · AZ · AGE
 * L7.13: SNR is now the real radio SNR in dB, computed by
 * js8_sync.c::compute_snr_db() from the 21 Costas sync-tone positions
 * in the ft8_lib waterfall (uses gfsk8's xsig/xbase formula). Color
 * thresholds: ≥-10 green (strong), -20..-10 yellow (workable),
 * <-20 gray (marginal).
 *
 * Navigation in the right-cycle (HOME→SETUP→HEARD→DIRECTED→HOME):
 *   TRACKBALL_LEFT  → SETUP
 *   TRACKBALL_RIGHT → DIRECTED
 *   TRACKBALL_UP    → scroll up
 *   TRACKBALL_DOWN  → scroll down
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "activity.h"

#include "esp_log.h"
#include "esp_timer.h"     // L7.9 build fix: esp_timer_get_time() for row-age
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "screen_heard";

// ── Layout — 320×240 landscape ───────────────────────────────────────
constexpr int SCREEN_W       = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H       = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H         = 16;
constexpr int FONT_W         = 8;
constexpr int PAD_X          = 4;

constexpr int HEADER_Y       = 6;
constexpr int SEPARATOR_Y    = HEADER_Y + FONT_H + 4;

// Column-header band, then list rows below.
constexpr int COL_HDR_Y      = SEPARATOR_Y + 6;
constexpr int FIRST_ROW_Y    = COL_HDR_Y + FONT_H + 4;
constexpr int ROW_STRIDE     = FONT_H;          // tight 16 px rows

// Column X positions — chosen to fit on 320 px wide font (8 px advance).
//   CALL  8 chars  @  4    width  64 px
//   SNR   3 chars  @  76   width  24 px   (reserved, blank for now)
//   GRID  6 chars  @ 108   width  48 px
//   MI    5 chars  @ 164   width  40 px   (right-aligned look)
//   AZ    3 chars  @ 212   width  24 px
//   AGE   5 chars  @ 244   width  40 px
constexpr int COL_CALL_X = PAD_X;            // 4
constexpr int COL_SNR_X  = 76;
constexpr int COL_GRID_X = 108;
constexpr int COL_MI_X   = 164;
constexpr int COL_AZ_X   = 212;
constexpr int COL_AGE_X  = 244;

constexpr int FOOTER_Y   = SCREEN_H - FONT_H - 6;

// Visible rows below the column header.
constexpr int MAX_VISIBLE_ROWS =
    (FOOTER_Y - FIRST_ROW_Y - 4) / ROW_STRIDE;

constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_FG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_COL_HDR     = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_HINT        = NANOJS8_COLOR_DARK_GRAY;

// Age-based row colors (in seconds-since-decode).
constexpr uint32_t AGE_FRESH_S = 30 * 60;       // 30 minutes → green
constexpr uint32_t AGE_RECENT_S = 4 * 60 * 60;  // 4 hours    → yellow
constexpr uint16_t COL_AGE_FRESH  = NANOJS8_COLOR_GREEN;
constexpr uint16_t COL_AGE_RECENT = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_AGE_OLD    = NANOJS8_COLOR_GRAY;

// ── State ────────────────────────────────────────────────────────────
int s_scroll_offset = 0;            // index of first visible row
uint32_t s_last_total = 0xFFFFFFFFu; // for tracking changes
uint32_t s_last_render_painted_count = 0;

// L7.10: dirty detection — skip render when nothing the operator can
// see has changed. Tracks (count, newest at_boot_s, scroll_offset).
// L7.10-fix2: minute is included so freshness colors AND the age column
// refresh at the 30-min / 4-h boundaries even on quiet bands.
uint32_t s_last_newest_at_s = 0;
int      s_last_scroll      = -1;
uint32_t s_last_minute      = 0xFFFFFFFFu;

// Trackball scroll tick divider — matches SETUP screen's pattern so a
// natural roll moves one row at a time.
constexpr int SCROLL_TICK_THRESHOLD = 4;
// L7.10: identical threshold for LEFT/RIGHT screen switches — prevents
// a single trackball flick (which fires multiple pulses at the 50ms
// poll rate) from cascading through every screen in the cycle.
constexpr int SWITCH_TICK_THRESHOLD = 4;
int s_up_ticks    = 0;
int s_down_ticks  = 0;
int s_left_ticks  = 0;
int s_right_ticks = 0;

// ── Drawing helpers ──────────────────────────────────────────────────

uint16_t color_for_age(uint32_t age_s) {
    if (age_s < AGE_FRESH_S)  return COL_AGE_FRESH;
    if (age_s < AGE_RECENT_S) return COL_AGE_RECENT;
    return COL_AGE_OLD;
}

void format_age(char *buf, size_t cap, uint32_t age_s) {
    // "now", "Ns", "Nm", "Nh", "Nd"
    if (age_s < 5)        snprintf(buf, cap, "now");
    else if (age_s < 60)  snprintf(buf, cap, "%us",  (unsigned)age_s);
    else if (age_s < 3600) snprintf(buf, cap, "%um", (unsigned)(age_s / 60));
    else if (age_s < 86400) snprintf(buf, cap, "%uh", (unsigned)(age_s / 3600));
    else                  snprintf(buf, cap, "%ud",   (unsigned)(age_s / 86400));
}

void paint_chrome() {
    nanojs8_display_clear(COL_BG);

    // Header band: "NanoJS8" + "HEARD" (right-anchored)
    nanojs8_display_draw_text(PAD_X, HEADER_Y, "NanoJS8",
                              COL_HEADER_NAME, COL_BG);
    const char *tag = "HEARD";
    int tag_w = nanojs8_display_text_width(tag);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - tag_w, HEADER_Y, tag,
                              COL_HEADER_FG, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);

    // Column header row
    nanojs8_display_draw_text(COL_CALL_X, COL_HDR_Y, "CALL",  COL_COL_HDR, COL_BG);
    nanojs8_display_draw_text(COL_SNR_X,  COL_HDR_Y, "SNR",   COL_COL_HDR, COL_BG);
    nanojs8_display_draw_text(COL_GRID_X, COL_HDR_Y, "GRID",  COL_COL_HDR, COL_BG);
    nanojs8_display_draw_text(COL_MI_X,   COL_HDR_Y, "MI",    COL_COL_HDR, COL_BG);
    nanojs8_display_draw_text(COL_AZ_X,   COL_HDR_Y, "AZ",    COL_COL_HDR, COL_BG);
    nanojs8_display_draw_text(COL_AGE_X,  COL_HDR_Y, "AGE",   COL_COL_HDR, COL_BG);

    // Separator under column header
    nanojs8_display_fill_rect(0, COL_HDR_Y + FONT_H + 1, SCREEN_W, 1,
                              COL_SEPARATOR);
}

void paint_row(int y, const nanojs8_activity_heard_t &h, uint32_t now_b) {
    // Erase row band first (so a shorter callsign doesn't leave fragments
    // from a longer previous one).
    nanojs8_display_fill_rect(0, y, SCREEN_W, ROW_STRIDE, COL_BG);

    const uint32_t age_s =
        (now_b > h.at_boot_s) ? (now_b - h.at_boot_s) : 0;
    const uint16_t row_color = color_for_age(age_s);

    // CALL — up to 8 chars to fit column width.
    char call_buf[9];
    snprintf(call_buf, sizeof(call_buf), "%-8.8s", h.callsign);
    nanojs8_display_draw_text(COL_CALL_X, y, call_buf, row_color, COL_BG);

    // SNR — L7.13: real radio SNR from compute_snr_db in js8_sync.
    // Color-coded by signal strength so the operator can scan the
    // table for workable stations at a glance. SNR_NA renders blank
    // (matches pre-L7.13 visual when the value isn't yet computed,
    // e.g. a HEARD slot freshly evicted but not yet repopulated).
    char snr_buf[8];
    uint16_t snr_color = row_color;
    if (h.last_snr_db == NANOJS8_ACTIVITY_SNR_NA) {
        snprintf(snr_buf, sizeof(snr_buf), "   ");
    } else {
        // %+03d → "-60".."-09","+00","+03","+30" — always 3 chars.
        snprintf(snr_buf, sizeof(snr_buf), "%+03d", (int)h.last_snr_db);
        // Thresholds match operator intuition on JS8:
        //   ≥ -10 dB : strong, easy QSO        → green
        //   -20..-10 : workable                → yellow
        //   < -20    : marginal, near floor    → gray
        if      (h.last_snr_db >= -10) snr_color = NANOJS8_COLOR_GREEN;
        else if (h.last_snr_db >= -20) snr_color = NANOJS8_COLOR_YELLOW;
        else                            snr_color = NANOJS8_COLOR_GRAY;
    }
    nanojs8_display_draw_text(COL_SNR_X, y, snr_buf, snr_color, COL_BG);

    // GRID — show 4 or 6 chars, blank if unknown.
    char grid_buf[7];
    if (h.grid[0]) snprintf(grid_buf, sizeof(grid_buf), "%-6.6s", h.grid);
    else           snprintf(grid_buf, sizeof(grid_buf), "%-6s",   "----");
    nanojs8_display_draw_text(COL_GRID_X, y, grid_buf, row_color, COL_BG);

    // MI — distance in statute miles. Right-padded for column align.
    // Buffer sized for compiler's worst-case view of %5d (int can be up
    // to 11 chars including sign) — our actual values are 0..9999 but
    // -Werror=format-truncation analyses the type range, not ours.
    char mi_buf[16];
    if (h.bearing_deg >= 0 && h.distance_mi > 0.0f) {
        if (h.distance_mi < 10000.0f) {
            snprintf(mi_buf, sizeof(mi_buf), "%5d",
                     (int)(h.distance_mi + 0.5f));
        } else {
            snprintf(mi_buf, sizeof(mi_buf), ">9999");
        }
    } else {
        snprintf(mi_buf, sizeof(mi_buf), "%5s", "");
    }
    nanojs8_display_draw_text(COL_MI_X, y, mi_buf, row_color, COL_BG);

    // AZ — bearing 0..359. Buffer headroom for compiler's int-range view.
    char az_buf[8];
    if (h.bearing_deg >= 0) {
        snprintf(az_buf, sizeof(az_buf), "%3d", (int)h.bearing_deg);
    } else {
        snprintf(az_buf, sizeof(az_buf), "%3s", "");
    }
    nanojs8_display_draw_text(COL_AZ_X, y, az_buf, row_color, COL_BG);

    // AGE — compact "now/Ns/Nm/Nh/Nd". Worst-case %u of uint32 is 10
    // chars, plus suffix and NUL — keep 16 for headroom.
    char age_buf[16];
    format_age(age_buf, sizeof(age_buf), age_s);
    nanojs8_display_draw_text(COL_AGE_X, y, age_buf, row_color, COL_BG);
}

void paint_empty_state() {
    // Friendly hint when we have nothing to show yet.
    const char *l1 = "no stations heard yet";
    const char *l2 = "tune to 7.078 MHz USB and wait";
    int w1 = nanojs8_display_text_width(l1);
    int w2 = nanojs8_display_text_width(l2);
    int y  = FIRST_ROW_Y + (MAX_VISIBLE_ROWS * ROW_STRIDE) / 2 - FONT_H;
    nanojs8_display_draw_text((SCREEN_W - w1) / 2, y, l1,
                              COL_HINT, COL_BG);
    nanojs8_display_draw_text((SCREEN_W - w2) / 2, y + FONT_H + 2, l2,
                              COL_HINT, COL_BG);
}

void paint_footer(uint32_t total) {
    nanojs8_display_fill_rect(0, FOOTER_Y - 2, SCREEN_W, FONT_H + 4, COL_BG);
    char buf[64];
    // ASCII-only — our 8x16 font only renders 0x20..0x7E.
    snprintf(buf, sizeof(buf),
             "%u heard   UP/DN scroll   L/R switch",
             (unsigned)total);
    nanojs8_display_draw_text(PAD_X, FOOTER_Y, buf, COL_FOOTER, COL_BG);
}

void on_enter() {
    ESP_LOGI(TAG, "Entering HEARD");
    s_scroll_offset = 0;
    s_last_total = 0xFFFFFFFFu;            // force full re-paint
    s_last_render_painted_count = 0;
    s_last_newest_at_s = 0;
    s_last_scroll      = -1;
    s_last_minute      = 0xFFFFFFFFu;
    s_up_ticks = s_down_ticks = s_left_ticks = s_right_ticks = 0;
    paint_chrome();
}

// Render-time snapshot buffer. Kept static (in BSS) so we don't burn
// ~2.5 KB of UI-task stack on every paint cycle. Safe because render()
// is never re-entrant.
nanojs8_activity_heard_t s_render_rows[NANOJS8_ACTIVITY_HEARD_MAX];

void render(bool /*full_redraw*/) {
    // Snapshot the heard table.
    const uint32_t n = nanojs8_activity_snapshot_heard(
        s_render_rows, sizeof(s_render_rows) / sizeof(s_render_rows[0]));

    // L7.10: dirty-detection — skip the redraw entirely if nothing
    // changed. snapshot_heard returns newest-first, so [0].at_boot_s
    // is the freshest; that plus count plus scroll offset summarizes
    // anything that could affect the painted output.
    // L7.10-fix2: shared timer read drives both the minute-boundary
    // dirty check and the per-row age math below.
    const uint32_t newest_at_s = (n > 0) ? s_render_rows[0].at_boot_s : 0;
    const uint32_t now_b       = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint32_t cur_minute  = now_b / 60u;
    if (n == s_last_total &&
        newest_at_s == s_last_newest_at_s &&
        s_scroll_offset == s_last_scroll &&
        cur_minute == s_last_minute) {
        return;
    }

    // Clamp scroll offset.
    int max_off = (int)n - MAX_VISIBLE_ROWS;
    if (max_off < 0) max_off = 0;
    if (s_scroll_offset > max_off) s_scroll_offset = max_off;
    if (s_scroll_offset < 0)        s_scroll_offset = 0;

    // Repaint rows. We blank-fill any rows that were previously painted
    // but are no longer in view (table shrank, or scroll changed).
    uint32_t painted = 0;
    for (int i = 0; i < MAX_VISIBLE_ROWS; ++i) {
        const int y = FIRST_ROW_Y + i * ROW_STRIDE;
        const int idx = s_scroll_offset + i;
        if (idx < (int)n) {
            paint_row(y, s_render_rows[idx], now_b);
            ++painted;
        } else {
            // Erase any leftover row content.
            nanojs8_display_fill_rect(0, y, SCREEN_W, ROW_STRIDE, COL_BG);
        }
    }

    if (n == 0) {
        // Show empty state hint in the row area.
        paint_empty_state();
    }

    paint_footer(n);

    s_last_total       = n;
    s_last_newest_at_s = newest_at_s;
    s_last_scroll      = s_scroll_offset;
    s_last_minute      = cur_minute;
    s_last_render_painted_count = painted;
}

bool handle_input(uint8_t event) {
    // L7.10: LEFT/RIGHT screen switches are tick-debounced so a single
    // trackball flick (which fires ~5-10 events at the 50ms poll rate)
    // doesn't cascade through every screen in the cycle.
    if (event == NANOJS8_TRACKBALL_LEFT) {
        s_right_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_SETUP);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        s_left_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            // L7.10: RIGHT now goes to ALL (was DIRECTED) since ALL
            // sits between HEARD and DIRECTED in the new 5-screen cycle.
            nanojs8_ui_set_screen(NANOJS8_SCREEN_ALL);
        }
        return true;
    }

    // Scroll with tick divider — matches SETUP focus pacing.
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
            ++s_scroll_offset;   // clamped in render()
        }
        return true;
    }

    return false;
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_HEARD = {
    .id           = NANOJS8_SCREEN_HEARD,
    .name         = "HEARD",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
