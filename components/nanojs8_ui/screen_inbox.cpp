/*
 * screen_inbox.cpp — L7.11g.3 INBOX screen
 * ==========================================
 *
 * Lists every entry held by nanojs8_mailbox. Shows all four lifecycle
 * states (UNREAD / READ / STORE / DELIVERED) in one unified view per
 * operator preference (vs filtering to inbound only). Visual encoding:
 *
 *   Type       Prefix   Call shown   Color (cursor inactive)
 *   --------   ------   ----------   -----------------------
 *   UNREAD     '>'      from_call    WHITE
 *   READ       ' '      from_call    GRAY (dim)
 *   STORE      '<'      to_call      YELLOW (we hold outbound obligation)
 *   DELIVERED  ' '      to_call      GRAY (done, kept for visibility)
 *
 * '>' means "incoming, look here"; '<' means "we're holding this for
 * an outbound peer". The call field shows the OTHER party — sender for
 * inbound, recipient for outbound — so the operator's eye lands on the
 * relevant callsign regardless of direction.
 *
 * Layout (320×240 landscape, 8×16 font, 40 char-columns):
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ INBOX                            3u/12  HH:MM        │  header
 *   │ ────────────────────────────────────────              │
 *   │ > KB1MCT      -09  18:42                              │  6 rows
 *   │   Hello there how have you been today...              │  × 32 px
 *   │ < KW3KW       ---  18:38                              │  = 192 px
 *   │   73 see you again later when we meet                 │
 *   │   ...                                                 │
 *   │ ────────────────────────────────────────              │
 *   │ 3 unread / 12 total · Enter read · D delete           │  footer
 *   └──────────────────────────────────────────────────────┘
 *
 * Navigation:
 *   TRACKBALL_LEFT   → DIRECTED   (tick-debounced)
 *   TRACKBALL_RIGHT  → COMPOSE    (tick-debounced)
 *   TRACKBALL_UP     → move cursor up   (single-tick — list nav is fine-grained)
 *   TRACKBALL_DOWN   → move cursor down
 *   ENTER (0x0D)     → open INBOX_DETAIL on selected entry; if entry
 *                       is UNREAD, auto-mark_read() before transitioning
 *   D / d            → DELETE the selected entry. Two-press confirmation:
 *                       first press arms "pending delete" mode (changes
 *                       footer to "Press D again · any other key cancels");
 *                       second D within 5 s commits delete; any other key
 *                       cancels.
 *
 * The cursor highlight is a 2-px cyan bar to the left of the selected
 * row's first line — minimal SPI cost (one fill_rect per render of the
 * dynamic region) and stays out of the way of the row content.
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "mailbox.h"
#include "config.h"      // L7.11g.7: detect remote STORE via callsign compare
#include "time_source.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>     // L7.11g.7: strcasecmp for callsign compare

namespace {

constexpr const char *TAG = "screen_inbox";

// ── Layout (320×240) ─────────────────────────────────────────────────
constexpr int SCREEN_W   = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H   = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H     = 16;
constexpr int FONT_W     = 8;
constexpr int PAD_X      = 4;

constexpr int HEADER_Y    = 4;
constexpr int SEPARATOR_Y = HEADER_Y + FONT_H + 2;          // 22

constexpr int FIRST_ROW_Y = SEPARATOR_Y + 2;                // 24
constexpr int LINE_STRIDE = FONT_H;                         // 16
constexpr int ROW_STRIDE  = LINE_STRIDE * 2;                // 32 — 2 lines per row

// 6 rows × 32 px = 192. End-of-rows Y = 24 + 192 = 216. Footer fits at
// 218 (2 px gap), text ends at 218+16 = 234 — comfortably inside 240.
constexpr int MAX_VISIBLE_ROWS = 6;
constexpr int ROWS_END_Y    = FIRST_ROW_Y + MAX_VISIBLE_ROWS * ROW_STRIDE;
constexpr int FOOTER_SEP_Y  = ROWS_END_Y;                    // 216
constexpr int FOOTER_Y      = FOOTER_SEP_Y + 2;              // 218

// Cursor highlight bar: 2 px wide, full row height.
constexpr int CURSOR_X = 0;
constexpr int CURSOR_W = 2;

// Column positions (8-px font advance, 40 cols):
constexpr int COL_GLYPH_X = PAD_X;                          // 4
constexpr int COL_CALL_X  = COL_GLYPH_X + 2 * FONT_W;       // 20 — '> ' then call
constexpr int COL_SNR_X   = COL_CALL_X  + 12 * FONT_W;      // 116 — 12 chars for callsign field
constexpr int COL_TIME_X  = COL_SNR_X   + 5 * FONT_W;       // 156 — gap of 2 chars after SNR
constexpr int COL_BODY_X  = COL_GLYPH_X + 1 * FONT_W;       // 12 — body preview indents under call

// Max chars per row of body preview. (SCREEN_W - COL_BODY_X) / FONT_W
// rounded down. (320 - 12) / 8 = 38.5 → 38 chars. Matches spec.
constexpr int BODY_PREVIEW_MAX_CHARS = (SCREEN_W - COL_BODY_X) / FONT_W;

// ── Colors ───────────────────────────────────────────────────────────
constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_FG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_BADGE_HOT   = NANOJS8_COLOR_YELLOW;  // unread > 0
constexpr uint16_t COL_BADGE_COLD  = NANOJS8_COLOR_GRAY;    // 0 unread
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_FOOTER_WARN = NANOJS8_COLOR_YELLOW;   // confirm-delete prompt
constexpr uint16_t COL_CURSOR      = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_EMPTY_HINT  = NANOJS8_COLOR_DARK_GRAY;

// Type-specific row colors (when cursor is NOT on this row).
constexpr uint16_t COL_UNREAD    = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_READ      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_STORE     = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_DELIVERED = NANOJS8_COLOR_GRAY;
// L7.11g.7: STORE entries we're holding for a remote operator
// (i.e., we received MSG TO:<other> from a third party and stored
// the body for delivery). Distinct from local STORE entries that
// the operator composed themselves. Magenta gives a clear "relay"
// visual cue without competing with the brighter STORE yellow.
constexpr uint16_t COL_REMOTE_STORE = NANOJS8_COLOR_MAGENTA;

// Freshness palette — same thresholds as HEARD/ALL/DIRECTED for visual
// consistency. Only applied to the timestamp column.
constexpr uint32_t AGE_FRESH_S  = 30u * 60u;
constexpr uint32_t AGE_RECENT_S = 4u * 60u * 60u;
constexpr uint16_t COL_AGE_FRESH  = NANOJS8_COLOR_GREEN;
constexpr uint16_t COL_AGE_RECENT = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_AGE_OLD    = NANOJS8_COLOR_GRAY;

// ── Tick thresholds ──────────────────────────────────────────────────
constexpr int SWITCH_TICK_THRESHOLD = 4;  // matches HEARD/DIRECTED/ALL

// Cursor-move threshold: 2 ticks gives a perceptible "click" per row,
// but doesn't require holding the trackball. 1 would feel too sensitive
// (one flick = 4-6 rows), 4 too sluggish.
constexpr int CURSOR_TICK_THRESHOLD = 2;

// Confirm-delete window — second D must arrive within this many us.
constexpr int64_t CONFIRM_DELETE_WINDOW_US = 5LL * 1000 * 1000;   // 5 s

// ── State (BSS) ──────────────────────────────────────────────────────
nanojs8_mailbox_entry_t s_snapshot[NANOJS8_MAILBOX_MAX];
uint32_t                s_snapshot_count = 0;

// Cursor index into s_snapshot (NOT a slot index). 0 = first visible
// row = newest by id. Clamped to [0, s_snapshot_count-1] at render.
int s_cursor = 0;

// Scroll offset: first visible row index. Clamped so cursor stays
// visible.
int s_scroll = 0;

int s_left_ticks   = 0;
int s_right_ticks  = 0;
int s_up_ticks     = 0;
int s_down_ticks   = 0;

// Confirm-delete state. id == 0 means "no pending". When non-zero, we
// remember the id so a delete on a stale cursor (e.g. background add
// shifted indices) deletes the entry the operator actually saw, not
// whatever happens to sit under the cursor now.
uint16_t s_pending_delete_id     = 0;
int64_t  s_pending_delete_at_us  = 0;

// Dirty-detection: render() repaints content only when something the
// user sees has changed (snapshot count/newest-id, cursor, scroll,
// pending-delete state, current minute for freshness boundaries).
uint32_t s_last_count        = 0xFFFFFFFFu;
uint16_t s_last_newest_id    = 0;
uint32_t s_last_unread_count = 0xFFFFFFFFu;
int      s_last_cursor       = -1;
int      s_last_scroll       = -1;
uint16_t s_last_pending_id   = 0xFFFF;   // sentinel != 0 and != real ids
uint32_t s_last_minute       = 0xFFFFFFFFu;

// ── Helpers ──────────────────────────────────────────────────────────

uint16_t color_for_age(uint32_t age_s) {
    if (age_s < AGE_FRESH_S)  return COL_AGE_FRESH;
    if (age_s < AGE_RECENT_S) return COL_AGE_RECENT;
    return COL_AGE_OLD;
}

uint16_t color_for_type(uint8_t type) {
    switch (type) {
        case NANOJS8_MAILBOX_TYPE_UNREAD:    return COL_UNREAD;
        case NANOJS8_MAILBOX_TYPE_READ:      return COL_READ;
        case NANOJS8_MAILBOX_TYPE_STORE:     return COL_STORE;
        case NANOJS8_MAILBOX_TYPE_DELIVERED: return COL_DELIVERED;
        default:                              return COL_READ;
    }
}

// L7.11g.7: a STORE row is a "remote store" (relay we're holding
// for a third party) when its from_call differs from our callsign.
// Locally-composed STOREs always have from_call == our_callsign.
// Case-insensitive compare. If our callsign isn't configured the
// check returns false so we don't mis-flag anything.
bool is_remote_store(const nanojs8_mailbox_entry_t *e) {
    if (!e || e->type != NANOJS8_MAILBOX_TYPE_STORE) return false;
    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (!cfg || cfg->callsign[0] == '\0') return false;
    return strcasecmp(e->from_call, cfg->callsign) != 0;
}

// Color for a row, taking remote-store distinction into account.
// All non-STORE rows go through color_for_type; STORE rows get
// the magenta highlight if they're remote-store.
uint16_t color_for_entry(const nanojs8_mailbox_entry_t *e) {
    if (is_remote_store(e)) return COL_REMOTE_STORE;
    return color_for_type(e->type);
}

// Prefix glyph + the call shown for this entry type.
char glyph_for_type(uint8_t type) {
    switch (type) {
        case NANOJS8_MAILBOX_TYPE_UNREAD:    return '>';
        case NANOJS8_MAILBOX_TYPE_STORE:     return '<';
        case NANOJS8_MAILBOX_TYPE_READ:
        case NANOJS8_MAILBOX_TYPE_DELIVERED:
        default:                              return ' ';
    }
}

// Inbound (UNREAD/READ) shows the sender; outbound-pending (STORE/
// DELIVERED) shows the recipient we're holding for / delivered to.
const char *call_for_entry(const nanojs8_mailbox_entry_t *e) {
    switch (e->type) {
        case NANOJS8_MAILBOX_TYPE_UNREAD:
        case NANOJS8_MAILBOX_TYPE_READ:
            return e->from_call;
        case NANOJS8_MAILBOX_TYPE_STORE:
        case NANOJS8_MAILBOX_TYPE_DELIVERED:
            return e->to_call;
        default:
            return e->from_call;
    }
}

// Returns age in seconds from "now". Wraps gracefully at midnight UTC
// rollover — if entry's seconds_today is higher than now's, we assume
// it was created "yesterday" and compute (86400 - entry + now). This
// over-estimates after a multi-day gap; without GPS-derived date we
// can't do better. Acceptable visual fuzziness past 24 h.
uint32_t age_seconds_from(uint32_t entry_s, uint32_t now_s) {
    if (entry_s <= now_s) {
        return now_s - entry_s;
    } else {
        return (86400u - entry_s) + now_s;
    }
}

// Truncate body into a single-line preview with ellipsis. Replaces
// newlines and tabs with spaces so wrapping doesn't matter.
void make_preview(char *dst, size_t dst_n, const char *src) {
    if (dst_n == 0) return;
    const size_t want = (size_t)BODY_PREVIEW_MAX_CHARS;
    const size_t cap  = (dst_n - 1 < want) ? (dst_n - 1) : want;

    size_t i = 0;
    for (; i < cap && src && src[i] != '\0'; ++i) {
        const char c = src[i];
        dst[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }

    // If we hit the cap mid-string, replace last 3 chars with "..."
    if (i == cap && src && src[i] != '\0' && cap >= 3) {
        dst[cap - 3] = '.';
        dst[cap - 2] = '.';
        dst[cap - 1] = '.';
    }
    dst[i] = '\0';
}

// ── Refresh the snapshot from the mailbox ────────────────────────────
void refresh_snapshot() {
    s_snapshot_count = nanojs8_mailbox_snapshot(s_snapshot,
                                                  NANOJS8_MAILBOX_MAX);
    if (s_snapshot_count == 0) {
        s_cursor = 0;
        s_scroll = 0;
        return;
    }
    if (s_cursor >= (int)s_snapshot_count) s_cursor = (int)s_snapshot_count - 1;
    if (s_cursor < 0) s_cursor = 0;

    // Keep cursor visible in the [scroll, scroll+visible-1] window.
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + MAX_VISIBLE_ROWS) {
        s_scroll = s_cursor - MAX_VISIBLE_ROWS + 1;
    }
    if (s_scroll < 0) s_scroll = 0;
}

// ── Painting ─────────────────────────────────────────────────────────

void paint_header(uint32_t unread, uint32_t total) {
    nanojs8_display_fill_rect(0, HEADER_Y - 2, SCREEN_W, FONT_H + 4, COL_BG);
    nanojs8_display_draw_text(PAD_X, HEADER_Y, "INBOX",
                                COL_HEADER_NAME, COL_BG);

    // Right-anchored badge: e.g. "3u/12"
    char badge[16];
    snprintf(badge, sizeof(badge), "%uu/%u",
             (unsigned)unread, (unsigned)total);
    const int badge_w = nanojs8_display_text_width(badge);
    const uint16_t badge_col = unread > 0 ? COL_BADGE_HOT : COL_BADGE_COLD;
    nanojs8_display_draw_text(SCREEN_W - PAD_X - badge_w, HEADER_Y,
                                badge, badge_col, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
}

// Erase the body area (rows + footer separator) without touching the
// header. Caller will repaint dynamic content.
void clear_body() {
    nanojs8_display_fill_rect(0, SEPARATOR_Y + 1,
                                SCREEN_W,
                                FOOTER_Y - (SEPARATOR_Y + 1),
                                COL_BG);
}

void paint_empty_state() {
    const char *msg1 = "Mailbox is empty.";
    const char *msg2 = "Decoded MSGs and outgoing STORE";
    const char *msg3 = "entries land here.";
    const int y1 = FIRST_ROW_Y + 2 * ROW_STRIDE;
    const int y2 = y1 + LINE_STRIDE + 4;
    const int y3 = y2 + LINE_STRIDE;
    int w;
    w = nanojs8_display_text_width(msg1);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, y1, msg1,
                                COL_EMPTY_HINT, COL_BG);
    w = nanojs8_display_text_width(msg2);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, y2, msg2,
                                COL_EMPTY_HINT, COL_BG);
    w = nanojs8_display_text_width(msg3);
    nanojs8_display_draw_text((SCREEN_W - w) / 2, y3, msg3,
                                COL_EMPTY_HINT, COL_BG);
}

void paint_row(int slot, const nanojs8_mailbox_entry_t *e,
                uint32_t now_s, bool cursor_here) {
    const int y_line1 = FIRST_ROW_Y + slot * ROW_STRIDE;
    const int y_line2 = y_line1 + LINE_STRIDE;

    // Cursor bar (cyan strip at the very left, spans both lines).
    if (cursor_here) {
        nanojs8_display_fill_rect(CURSOR_X, y_line1,
                                    CURSOR_W, ROW_STRIDE,
                                    COL_CURSOR);
    }

    const uint16_t row_col = color_for_entry(e);
    const char     glyph   = glyph_for_type(e->type);
    const char    *call    = call_for_entry(e);

    // Line 1: glyph + call + SNR + HH:MM
    char gbuf[2] = { glyph, '\0' };
    nanojs8_display_draw_text(COL_GLYPH_X, y_line1, gbuf,
                                row_col, COL_BG);

    // Call: truncate to 11 chars so it fits the 12-char column
    // including a trailing space.
    char call_buf[12];
    size_t i = 0;
    for (; i < sizeof(call_buf) - 1 && call[i] != '\0'; ++i) {
        call_buf[i] = call[i];
    }
    call_buf[i] = '\0';
    nanojs8_display_draw_text(COL_CALL_X, y_line1, call_buf,
                                row_col, COL_BG);

    // SNR — only meaningful for inbound entries. STORE/DELIVERED show
    // "---".
    char snr_buf[6];
    if (e->type == NANOJS8_MAILBOX_TYPE_UNREAD ||
        e->type == NANOJS8_MAILBOX_TYPE_READ) {
        snprintf(snr_buf, sizeof(snr_buf), "%+03d", (int)e->snr_db);
    } else {
        snprintf(snr_buf, sizeof(snr_buf), "---");
    }
    nanojs8_display_draw_text(COL_SNR_X, y_line1, snr_buf,
                                row_col, COL_BG);

    // HH:MM — colored by freshness if UTC is set. If UTC not set, show
    // "--:--" in dim gray.
    char time_buf[6];
    uint16_t time_col;
    if (nanojs8_time_is_set()) {
        const uint32_t s = e->utc_seconds_today % 86400u;
        snprintf(time_buf, sizeof(time_buf),
                 "%02u:%02u", (unsigned)(s / 3600u),
                 (unsigned)((s % 3600u) / 60u));
        time_col = color_for_age(age_seconds_from(e->utc_seconds_today,
                                                    now_s));
        // Apply a slight dim for READ/DELIVERED so the freshness color
        // still reads but doesn't compete with bright UNREAD/STORE.
        if (e->type == NANOJS8_MAILBOX_TYPE_READ ||
            e->type == NANOJS8_MAILBOX_TYPE_DELIVERED) {
            time_col = COL_READ;
        }
    } else {
        snprintf(time_buf, sizeof(time_buf), "--:--");
        time_col = COL_READ;
    }
    nanojs8_display_draw_text(COL_TIME_X, y_line1, time_buf,
                                time_col, COL_BG);

    // Line 2: "#<id> <body preview>" indented under the call.
    // L7.11g.6-fix1: prefix the entry's id so operators see at a
    // glance what to give peers in "QUERY MSG <id>" requests. Compose
    // into a single buffer and let make_preview() handle the
    // 38-char-wide truncation uniformly — keeps a stable visual
    // edge regardless of id width.
    char composed[NANOJS8_MAILBOX_BODY_LEN + 12];
    snprintf(composed, sizeof(composed), "#%u %s",
             (unsigned)e->id, e->body);
    char preview[64];
    make_preview(preview, sizeof(preview), composed);
    nanojs8_display_draw_text(COL_BODY_X, y_line2, preview,
                                row_col, COL_BG);
}

void paint_footer(uint32_t unread, uint32_t total, bool pending_delete) {
    nanojs8_display_fill_rect(0, FOOTER_SEP_Y, SCREEN_W, 1, COL_SEPARATOR);
    nanojs8_display_fill_rect(0, FOOTER_Y, SCREEN_W, FONT_H, COL_BG);

    if (pending_delete) {
        // 40-char width is ~SCREEN_W; this msg is 38 chars, fits.
        const char *msg = "Press D again to confirm  any key cancels";
        nanojs8_display_draw_text(PAD_X, FOOTER_Y, msg,
                                    COL_FOOTER_WARN, COL_BG);
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf),
             "%u unread / %u total  Enter read  D delete",
             (unsigned)unread, (unsigned)total);
    nanojs8_display_draw_text(PAD_X, FOOTER_Y, buf,
                                COL_FOOTER, COL_BG);
}

// ── Render entry points ──────────────────────────────────────────────

void render(bool full_redraw) {
    refresh_snapshot();

    const uint32_t total  = s_snapshot_count;
    const uint32_t unread = nanojs8_mailbox_count_unread();

    // Compute "now" for freshness coloring.
    const uint32_t now_s = nanojs8_time_is_set()
                              ? nanojs8_time_seconds_today() : 0;
    const uint32_t cur_minute = now_s / 60u;

    // Newest-id changes whenever the top entry changes (eviction,
    // addition, or first-time fill). Used for cheap dirty detection.
    const uint16_t newest_id = (s_snapshot_count > 0)
                                  ? s_snapshot[0].id : 0;

    const bool pending_delete = (s_pending_delete_id != 0);

    const bool changed =
        full_redraw ||
        s_last_count        != total ||
        s_last_newest_id    != newest_id ||
        s_last_unread_count != unread ||
        s_last_cursor       != s_cursor ||
        s_last_scroll       != s_scroll ||
        s_last_pending_id   != s_pending_delete_id ||
        s_last_minute       != cur_minute;
    if (!changed) return;

    if (full_redraw) {
        nanojs8_display_fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    }

    paint_header(unread, total);

    clear_body();

    if (total == 0) {
        paint_empty_state();
    } else {
        // Paint visible rows.
        const int first = s_scroll;
        const int last  = first + MAX_VISIBLE_ROWS;  // exclusive
        for (int idx = first; idx < (int)total && idx < last; ++idx) {
            const int slot = idx - first;
            paint_row(slot, &s_snapshot[idx], now_s, idx == s_cursor);
        }
    }

    paint_footer(unread, total, pending_delete);

    s_last_count        = total;
    s_last_newest_id    = newest_id;
    s_last_unread_count = unread;
    s_last_cursor       = s_cursor;
    s_last_scroll       = s_scroll;
    s_last_pending_id   = s_pending_delete_id;
    s_last_minute       = cur_minute;
}

void on_enter() {
    s_left_ticks  = 0;
    s_right_ticks = 0;
    s_up_ticks    = 0;
    s_down_ticks  = 0;
    // Don't reset cursor on re-entry — keep position the operator had.
    // But DO drop any pending delete so a re-entry can't accidentally
    // commit a delete the operator forgot about.
    s_pending_delete_id    = 0;
    s_pending_delete_at_us = 0;
    // Force a full repaint on the next render().
    s_last_count = 0xFFFFFFFFu;
    ESP_LOGI(TAG, "Entering INBOX (cursor=%d snapshot=%u)",
             s_cursor, (unsigned)s_snapshot_count);
}

// Clear pending-delete on any non-D input.
void clear_pending_delete(const char *reason) {
    if (s_pending_delete_id == 0) return;
    ESP_LOGI(TAG, "pending delete (id=%u) cancelled: %s",
             (unsigned)s_pending_delete_id, reason);
    s_pending_delete_id    = 0;
    s_pending_delete_at_us = 0;
}

// ── Selected-entry helpers ──────────────────────────────────────────

uint16_t selected_id() {
    if (s_snapshot_count == 0) return 0;
    if (s_cursor < 0 || s_cursor >= (int)s_snapshot_count) return 0;
    return s_snapshot[s_cursor].id;
}

// ── Internal helper for INBOX_DETAIL to know which entry to open ─────
// Exposed via a C-linkage function below so INBOX_DETAIL doesn't have
// to peek into our anonymous-namespace state. Returns 0 if no row
// selected (empty mailbox or invalid cursor).
} // anonymous namespace

extern "C" uint16_t nanojs8_screen_inbox_selected_id(void) {
    return selected_id();
}

namespace {

// ── Input handler ────────────────────────────────────────────────────

bool handle_input(uint8_t event) {
    // Check pending-delete expiry on every event so a 5-s pause auto-
    // cancels even before the operator presses a key.
    if (s_pending_delete_id != 0) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_pending_delete_at_us > CONFIRM_DELETE_WINDOW_US) {
            clear_pending_delete("timeout");
        }
    }

    // Ring navigation. LEFT → DIRECTED, RIGHT → COMPOSE. Tick-debounced
    // because trackball flicks fire 5-10 events at the 50 ms poll rate.
    if (event == NANOJS8_TRACKBALL_LEFT) {
        clear_pending_delete("LEFT");
        s_right_ticks = s_up_ticks = s_down_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_DIRECTED);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        clear_pending_delete("RIGHT");
        s_left_ticks = s_up_ticks = s_down_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_COMPOSE);
        }
        return true;
    }

    // Cursor navigation. Cheaper tick threshold than screen switching
    // (2 vs 4) so one flick moves through a few rows comfortably.
    if (event == NANOJS8_TRACKBALL_UP) {
        clear_pending_delete("UP");
        s_down_ticks = 0;
        if (++s_up_ticks >= CURSOR_TICK_THRESHOLD) {
            s_up_ticks = 0;
            if (s_cursor > 0) --s_cursor;
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_DOWN) {
        clear_pending_delete("DOWN");
        s_up_ticks = 0;
        if (++s_down_ticks >= CURSOR_TICK_THRESHOLD) {
            s_down_ticks = 0;
            if (s_cursor < (int)s_snapshot_count - 1) ++s_cursor;
        }
        return true;
    }

    // CLICK acts as ENTER — opens detail view of the selected entry.
    if (event == NANOJS8_TRACKBALL_CLICK || event == 0x0D /*ENTER*/) {
        clear_pending_delete("ENTER");
        const uint16_t id = selected_id();
        if (id == 0) {
            ESP_LOGW(TAG, "ENTER: no entry to open (snapshot=%u cursor=%d)",
                     (unsigned)s_snapshot_count, s_cursor);
            return true;
        }
        // Auto-mark UNREAD → READ on open.
        if (s_cursor >= 0 &&
            s_cursor < (int)s_snapshot_count &&
            s_snapshot[s_cursor].type == NANOJS8_MAILBOX_TYPE_UNREAD) {
            const esp_err_t e = nanojs8_mailbox_mark_read(id);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "ENTER: mark_read(%u) returned %s",
                         (unsigned)id, esp_err_to_name(e));
                // Non-fatal — detail screen still opens.
            }
        }
        nanojs8_ui_set_screen(NANOJS8_SCREEN_INBOX_DETAIL);
        return true;
    }

    // DELETE — two-press confirmation. D / d.
    if (event == 'D' || event == 'd') {
        const uint16_t cur_id = selected_id();
        if (cur_id == 0) {
            ESP_LOGW(TAG, "D: no entry to delete");
            return true;
        }
        if (s_pending_delete_id == cur_id) {
            // Second press — commit.
            const esp_err_t e = nanojs8_mailbox_delete(cur_id);
            if (e == ESP_OK) {
                ESP_LOGI(TAG, "Deleted id=%u (confirmed)",
                         (unsigned)cur_id);
            } else {
                ESP_LOGW(TAG, "delete(%u) returned %s",
                         (unsigned)cur_id, esp_err_to_name(e));
            }
            s_pending_delete_id    = 0;
            s_pending_delete_at_us = 0;
            // Snapshot rebuild and cursor clamp happens in next render.
        } else {
            // First press (or different id from previously armed) —
            // arm.
            s_pending_delete_id    = cur_id;
            s_pending_delete_at_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Delete armed for id=%u — awaiting confirmation",
                     (unsigned)cur_id);
        }
        return true;
    }

    // Any other key (including BACKSPACE) cancels pending delete and
    // is otherwise unhandled.
    clear_pending_delete("other key");
    return false;
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_INBOX = {
    .id           = NANOJS8_SCREEN_INBOX,
    .name         = "INBOX",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
