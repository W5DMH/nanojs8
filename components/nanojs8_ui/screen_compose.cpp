/*
 * screen_compose.cpp — L7.11f COMPOSE screen (multi-field TX form)
 * =================================================================
 *
 * Mirrors MicroJS8's COMPOSE screen (src/microjs8/ui/screens.py
 * _render_compose + src/microjs8/ui/state.py build_compose_wire).
 * Supports all 10 JS8 verbs:
 *
 *   FREE        <TO> <TEXT>                     no verb
 *   MSG         <TO> MSG <TEXT>                 buffered mail
 *   MSG TO      <TO> MSG TO:<FOR> <TEXT>        relay (extra FOR field)
 *   STORE       (local mailbox — no TX)         write-only in L7.11f
 *   AGN?        <TO> AGN?                       retransmit request
 *   SNR?        <TO> SNR?                       signal report request
 *   GRID?       <TO> GRID?                      grid query
 *   QUERY MSGS  <TO> QUERY MSGS                 ask peer for held msgs
 *   QUERY MSG   <TO> QUERY MSG <id>             fetch by numeric id
 *   MYLOC       <TO> MSG MYLOC <coords>         location via MSG (direct → ACK; broadcast → silent)
 *
 * Fields:
 *   TO    callsign or @group (free-typed or picked from HEARD/groups)
 *   CMD   verb dropdown (10 verbs above)
 *   FOR   only present when CMD == MSG TO
 *   TEXT  free-text body (typed)
 *   SEND  fire-button (TX or STORE depending on CMD)
 *
 * Two-mode UX (mirrors SETUP):
 *
 *   NAV mode (default after entering screen):
 *     trackball UP   : previous field
 *     trackball DOWN : next field
 *     trackball LEFT : ring nav to previous screen (DIRECTED)
 *     trackball RIGHT: ring nav to next screen (ALLCALL)
 *     trackball CLICK / Enter (0x0D): enter EDIT mode on focused field;
 *       on SEND, fire transmission/store immediately.
 *
 *   EDIT mode (entered via CLICK on a field):
 *     TO/FOR field:
 *       printable chars  : append to buffer (uppercased on commit)
 *       trackball UP/DOWN: cycle HEARD list + configured @groups
 *                          (newest HEARD callsigns first, then groups
 *                          marked with leading '*' in display only)
 *       Enter or CLICK   : commit + return to NAV
 *       ESC (0x1B)       : cancel (revert buffer) + return to NAV
 *       Backspace (0x08) : drop last char of buffer
 *     CMD field:
 *       trackball UP     : previous verb (wraps)
 *       trackball DOWN   : next verb (wraps)
 *       Enter or CLICK   : commit + return to NAV
 *       ESC              : cancel + return to NAV (revert verb)
 *     TEXT field:
 *       same as TO/FOR but no HEARD picker (UP/DOWN do nothing here)
 *
 * TX warnings (priority-ordered, displayed in footer right):
 *   1. TO empty                       → "TO callsign required"
 *   2. FOR empty (MSG TO only)        → "FOR callsign required"
 *   3. TO == own call (non-STORE)     → "TO cannot be your own call"
 *   4. MSG TO with FOR == TO          → "FOR cannot equal TO"
 *   5. QUERY MSG without numeric TEXT → "MSG ID must be a number"
 *   6. No active radio profile        → "TX OFF — configure radio"
 *   7. UTC not set (non-STORE)        → "TX OFF — set UTC in SETUP"
 *
 * STORE bypasses warnings 3, 6, 7 (local mailbox write doesn't need
 * TX gates). It still requires non-empty TO and TEXT.
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "config.h"
#include "tx_audio.h"
#include "mailbox.h"
#include "activity.h"
#include "time_source.h"
#include "radio.h"
#include "gps.h"          // L7.14-fix6: nanojs8_gps_format_position for MYLOC

#include "esp_log.h"
#include "esp_timer.h"   // L7.14-fix3: monotonic clock for transient error banner

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "screen_compose";

// ── Geometry (320×240) ───────────────────────────────────────────────
constexpr int SCREEN_W   = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H   = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H     = 16;
constexpr int FONT_W     = 8;
constexpr int PAD_X      = 6;

constexpr int HEADER_Y    = 6;
constexpr int SEPARATOR_Y = HEADER_Y + FONT_H + 4;

// Field rows. Heights tuned so all fields fit even with the optional
// FOR row inserted (worst case: TO + CMD + FOR + TEXT + SEND).
constexpr int FIELD_Y0    = SEPARATOR_Y + 8;
constexpr int ROW_H       = 24;
constexpr int TEXT_BOX_H  = 40;  // a bit taller for multi-line bodies

constexpr int FOOTER_Y    = SCREEN_H - FONT_H - 6;

// X coords inside a field row.
constexpr int LABEL_X     = PAD_X;
constexpr int VALUE_X     = PAD_X + 4 + 6 * FONT_W;  // after "LABEL "
                                                      // (5 chars + space)

// ── Colors ───────────────────────────────────────────────────────────
constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_HEADER_TAG  = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_LABEL_NORM  = NANOJS8_COLOR_GRAY;       // dim
constexpr uint16_t COL_LABEL_FOCUS = NANOJS8_COLOR_GREEN;      // bright
constexpr uint16_t COL_LABEL_EDIT  = NANOJS8_COLOR_YELLOW;     // editing
constexpr uint16_t COL_VALUE       = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_VALUE_DIM   = NANOJS8_COLOR_DARK_GRAY;  // empty
constexpr uint16_t COL_ACCENT_BG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_FOCUS_FG    = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_WARN        = NANOJS8_COLOR_RED;

// ── Sensitivity dividers ─────────────────────────────────────────────
constexpr int SWITCH_TICK_THRESHOLD = 4;  // screen-cycle stiffness
constexpr int FIELD_TICK_THRESHOLD  = 2;  // field-cycle stiffness
constexpr int PICKER_TICK_THRESHOLD = 2;  // picker / verb cycle stiffness

// ── CMD verb table ───────────────────────────────────────────────────
enum compose_cmd_t {
    CMD_FREE = 0,
    CMD_MSG,
    CMD_MSG_TO,
    CMD_STORE,
    CMD_AGN_Q,
    CMD_SNR_Q,
    CMD_GRID_Q,
    CMD_QUERY_MSGS,
    CMD_QUERY_MSG,
    CMD_MYLOC,
    CMD_COUNT
};

const char * const CMD_LABEL[CMD_COUNT] = {
    "FREE",       // empty verb on wire
    "MSG",
    "MSG TO",
    "STORE",
    "AGN?",
    "SNR?",
    "GRID?",
    "QUERY MSGS",
    "QUERY MSG",
    "MYLOC",
};

// ── Field IDs ────────────────────────────────────────────────────────
//
// Field set depends on CMD: MSG_TO inserts a FOR row between CMD and
// TEXT. We keep the field IDs stable (logical "what this is") and
// build a per-frame field cycle list that may or may not include FOR.
enum compose_field_t {
    FIELD_TO    = 0,
    FIELD_CMD   = 1,
    FIELD_FOR   = 2,   // only present when CMD == MSG TO
    FIELD_TEXT  = 3,
    FIELD_SEND  = 4,
    FIELD_COUNT
};

constexpr int   MAX_FIELD_CYCLE = FIELD_COUNT;
constexpr size_t TO_BUF_LEN     = 16;
constexpr size_t FOR_BUF_LEN    = 16;
constexpr size_t TEXT_BUF_LEN   = 160;

// Maximum entries in the HEARD/groups picker (we cap at 16 callsigns
// + 4 groups for screen-size sanity).
constexpr uint32_t PICKER_HEARD_MAX  = 16;
constexpr uint32_t PICKER_GROUPS_MAX = 4;
// L7.14-fix8: +1 for the always-present @ALLCALL entry (prepended in
// rebuild_picker_list). Without the bump, a 4-group operator with a
// full HEARD list would lose the oldest HEARD slot. Cost is one extra
// 64-byte slot in BSS.
constexpr uint32_t PICKER_TOTAL_MAX  = 1 + PICKER_HEARD_MAX + PICKER_GROUPS_MAX;

// ── Module state ─────────────────────────────────────────────────────
//
// Two buffer copies per text field: "committed" (what TX will use)
// and "edit" (what the operator is typing — visible in EDIT mode).
// On commit we copy edit→committed; on cancel we revert edit←committed.
char s_to_committed   [TO_BUF_LEN]   = {0};
char s_to_edit        [TO_BUF_LEN]   = {0};
char s_for_committed  [FOR_BUF_LEN]  = {0};
char s_for_edit       [FOR_BUF_LEN]  = {0};
char s_text_committed [TEXT_BUF_LEN] = {0};
char s_text_edit      [TEXT_BUF_LEN] = {0};

compose_cmd_t s_cmd_committed = CMD_FREE;
compose_cmd_t s_cmd_edit      = CMD_FREE;

// Picker state — when editing TO or FOR, ↑/↓ walks a flat list of
// [HEARD callsigns, group names]. -1 means "free typing" (no
// dropdown item selected). Otherwise index into the picker list.
int      s_picker_index = -1;
uint32_t s_picker_count = 0;
char     s_picker_items[PICKER_TOTAL_MAX][NANOJS8_CONFIG_GROUPS_LEN] = {0};
// Parallel flag: true if entry i is a group (display with leading '*')
bool     s_picker_is_group[PICKER_TOTAL_MAX] = {0};

// Mode state.
enum compose_mode_t {
    MODE_NAV  = 0,
    MODE_EDIT = 1,
};
compose_mode_t  s_mode         = MODE_NAV;
compose_field_t s_focused      = FIELD_TO;

// Tick counters for trackball sensitivity dividers.
int s_left_ticks   = 0;
int s_right_ticks  = 0;
int s_up_ticks     = 0;
int s_down_ticks   = 0;

// Dirty marker for render economy. Bumped on any state change.
uint32_t s_dirty_seq         = 0;
uint32_t s_last_rendered_seq = 0xFFFFFFFFu;

// L7.14-fix3: transient error banner state — mirrors screen_allcall
// from L7.13-fix3. When fire_send() refuses to TX for any reason
// (precondition failure, build_wire fail, transmit_text error,
// mailbox add_store error), we stash a short human-readable message
// here with a 4-second expiry. paint_footer renders it in red over
// the contextual hint until esp_timer passes the expiry. Operator
// no longer needs a serial console to discover why a SEND was
// rejected — same UX as ALLCALL hotkeys.
constexpr int64_t ERROR_BANNER_US = 4 * 1000 * 1000;
char    s_error_msg[64]   = {0};
int64_t s_error_until_us  = 0;

// ── Helpers ──────────────────────────────────────────────────────────

void mark_dirty() { ++s_dirty_seq; }

// L7.14-fix3: stash an error for paint_footer to render in red.
// Pass nullptr to clear (used after a successful send so the next
// SEND attempt starts with a clean footer). mark_dirty() forces a
// re-render so the banner appears immediately, not on the next
// trackball event.
void set_error(const char *msg) {
    if (!msg) {
        s_error_msg[0]   = '\0';
        s_error_until_us = 0;
        mark_dirty();
        return;
    }
    strncpy(s_error_msg, msg, sizeof(s_error_msg) - 1);
    s_error_msg[sizeof(s_error_msg) - 1] = '\0';
    s_error_until_us = esp_timer_get_time() + ERROR_BANNER_US;
    mark_dirty();
}

// Truncating, NUL-guaranteed string copy.
void safe_strncpy(char *dst, size_t dst_n, const char *src) {
    if (dst_n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = dst_n - 1;
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// In-place ASCII uppercase.
void uppercase_inplace(char *s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        *s = (char)toupper(c);
    }
}

bool is_msg_to() {
    return s_cmd_committed == CMD_MSG_TO;
}

// Field cycle order, dependent on whether MSG_TO is active. Returns
// the next/prev field in the cycle, wrapping. Fields not in the
// current cycle are skipped.
compose_field_t next_field(compose_field_t cur, bool forward) {
    const compose_field_t cycle_no_for[] = {
        FIELD_TO, FIELD_CMD, FIELD_TEXT, FIELD_SEND
    };
    const compose_field_t cycle_with_for[] = {
        FIELD_TO, FIELD_CMD, FIELD_FOR, FIELD_TEXT, FIELD_SEND
    };

    const compose_field_t *cycle;
    size_t n;
    if (is_msg_to()) {
        cycle = cycle_with_for;
        n     = sizeof(cycle_with_for) / sizeof(cycle_with_for[0]);
    } else {
        cycle = cycle_no_for;
        n     = sizeof(cycle_no_for) / sizeof(cycle_no_for[0]);
    }

    // Find current position; if cur is FOR but no longer in cycle,
    // fall back to start.
    size_t pos = 0;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        if (cycle[i] == cur) { pos = i; found = true; break; }
    }
    if (!found) {
        // Cur dropped out of cycle (e.g. operator left MSG_TO with
        // FOR focused). Land on TO as the safe default.
        return FIELD_TO;
    }
    if (forward) {
        pos = (pos + 1) % n;
    } else {
        pos = (pos + n - 1) % n;
    }
    return cycle[pos];
}

// Build the picker list (HEARD callsigns + configured groups) and
// store it in s_picker_items. Called when entering EDIT mode on
// TO/FOR field. Skips our own callsign from HEARD.
void rebuild_picker_list() {
    s_picker_count = 0;
    memset(s_picker_items, 0, sizeof(s_picker_items));
    memset(s_picker_is_group, 0, sizeof(s_picker_is_group));

    // L7.14-fix8: @ALLCALL as the first picker entry. Always present,
    // not tied to user config. Previously operators could only TX to
    // @ALLCALL via the dedicated ALLCALL screen's three fixed verbs
    // (CQ / QUERY MSGS / one more) — now any COMPOSE verb can target
    // @ALLCALL (e.g. MYLOC @ALLCALL for broadcast location). Dedup
    // below avoids a duplicate entry if the operator's groups CSV
    // also lists @ALLCALL.
    safe_strncpy(s_picker_items[s_picker_count],
                 NANOJS8_CONFIG_GROUPS_LEN, "@ALLCALL");
    s_picker_is_group[s_picker_count] = true;
    ++s_picker_count;

    // Then user-configured groups — operator's curated list of who
    // they actually talk to.
    char groups[PICKER_GROUPS_MAX][NANOJS8_CONFIG_GROUPS_LEN];
    uint32_t n_groups = nanojs8_config_groups_enumerate(
        groups, PICKER_GROUPS_MAX);
    for (uint32_t i = 0; i < n_groups
                          && s_picker_count < PICKER_TOTAL_MAX; ++i) {
        // L7.14-fix8: dedup against @ALLCALL (already added above).
        if (strcasecmp(groups[i], "@ALLCALL") == 0) continue;
        safe_strncpy(s_picker_items[s_picker_count],
                     NANOJS8_CONFIG_GROUPS_LEN, groups[i]);
        s_picker_is_group[s_picker_count] = true;
        ++s_picker_count;
    }

    // Then HEARD callsigns (newest first), excluding our own call.
    const nanojs8_config_t *cfg = nanojs8_config_get();
    const char *own = (cfg && cfg->callsign[0] != '\0')
                        ? cfg->callsign : "";

    nanojs8_activity_heard_t heard[PICKER_HEARD_MAX];
    uint32_t n_heard = nanojs8_activity_snapshot_heard(
        heard, PICKER_HEARD_MAX);
    for (uint32_t i = 0; i < n_heard
                          && s_picker_count < PICKER_TOTAL_MAX; ++i) {
        if (own[0] && strcasecmp(heard[i].callsign, own) == 0) {
            continue;  // skip our own call
        }
        safe_strncpy(s_picker_items[s_picker_count],
                     NANOJS8_CONFIG_GROUPS_LEN, heard[i].callsign);
        s_picker_is_group[s_picker_count] = false;
        ++s_picker_count;
    }
}

// Move picker selection; called on UP/DOWN in EDIT mode for TO/FOR.
// Cycling past the ends wraps. After cycling, copy the picker item
// into the edit buffer so it shows in the field.
void picker_cycle(bool forward, char *edit_buf, size_t edit_n) {
    if (s_picker_count == 0) return;
    if (s_picker_index < 0) {
        s_picker_index = forward ? 0 : (int)(s_picker_count - 1);
    } else {
        if (forward) {
            s_picker_index = (s_picker_index + 1)
                              % (int)s_picker_count;
        } else {
            s_picker_index = (s_picker_index - 1
                                + (int)s_picker_count)
                              % (int)s_picker_count;
        }
    }
    safe_strncpy(edit_buf, edit_n, s_picker_items[s_picker_index]);
    mark_dirty();
}

// On any printable typed char into TO/FOR/TEXT we revert picker
// state — operator is back to free typing.
void picker_reset() {
    s_picker_index = -1;
}

// ── Wire builder ─────────────────────────────────────────────────────
//
// Build the on-air wire string for the current state. Returns false
// if the compose is incomplete (caller already validated with
// compute_tx_warning(), but we double-check here as a safety net).
//
// Behavior matches MicroJS8 build_compose_wire().
bool build_wire(char *out, size_t out_n) {
    if (out_n < 32) return false;
    out[0] = '\0';

    // Uppercase normalized callsigns.
    char to[TO_BUF_LEN];
    safe_strncpy(to, sizeof(to), s_to_committed);
    uppercase_inplace(to);

    char for_call[FOR_BUF_LEN];
    safe_strncpy(for_call, sizeof(for_call), s_for_committed);
    uppercase_inplace(for_call);

    const char *text = s_text_committed;
    const nanojs8_config_t *cfg = nanojs8_config_get();
    const char *grid = (cfg && cfg->grid[0]) ? cfg->grid : "";

    if (to[0] == '\0') return false;  // TO required for every TX'ing verb

    switch (s_cmd_committed) {
        case CMD_FREE:
            if (text[0] == '\0') return false;
            snprintf(out, out_n, "%s %s", to, text);
            return true;
        case CMD_MSG:
            if (text[0] == '\0') return false;
            snprintf(out, out_n, "%s MSG %s", to, text);
            return true;
        case CMD_MSG_TO:
            if (for_call[0] == '\0' || text[0] == '\0') return false;
            snprintf(out, out_n, "%s MSG TO:%s %s",
                     to, for_call, text);
            return true;
        case CMD_STORE:
            // STORE has no wire form — caller routes to mailbox.
            return false;
        case CMD_AGN_Q:
            snprintf(out, out_n, "%s AGN?", to);
            return true;
        case CMD_SNR_Q:
            snprintf(out, out_n, "%s SNR?", to);
            return true;
        case CMD_GRID_Q:
            snprintf(out, out_n, "%s GRID?", to);
            return true;
        case CMD_QUERY_MSGS:
            snprintf(out, out_n, "%s QUERY MSGS", to);
            return true;
        case CMD_QUERY_MSG: {
            // Numeric id required (validated separately in
            // compute_tx_warning).
            if (text[0] == '\0') return false;
            for (const char *p = text; *p; ++p) {
                if (!isdigit((unsigned char)*p)) return false;
            }
            snprintf(out, out_n, "%s QUERY MSG %s", to, text);
            return true;
        }
        case CMD_MYLOC:
            // L7.14-fix8: emit as a MSG verb with a "MYLOC " body
            // prefix instead of a literal MYLOC verb. JS8Call understands
            // MSG and auto-ACKs when the receiver is the directed
            // callsign — so a direct MYLOC to a single station returns
            // an ACK confirming delivery. Broadcasts to @ALLCALL or a
            // @GROUP still go out but JS8Call (correctly) doesn't ACK
            // broadcasts. The "MYLOC " prefix is plain body text — JS8
            // doesn't parse it as a verb, but human operators (and our
            // own DIRECTED screen) see it and recognize the message as
            // location information. UI is unchanged: dropdown label
            // stays "MYLOC", TEXT field auto-fills with just the coords.
            if (text[0] == '\0') return false;
            snprintf(out, out_n, "%s MSG MYLOC %s", to, text);
            return true;
        default:
            return false;
    }
}

// ── TX warning calculator ────────────────────────────────────────────
//
// Returns nullptr (no warning) or a static string describing why TX
// is currently not possible. Priority-ordered to match MicroJS8.

const char *compute_tx_warning() {
    const bool store = (s_cmd_committed == CMD_STORE);
    const bool msg_to = is_msg_to();
    const bool query_msg = (s_cmd_committed == CMD_QUERY_MSG);

    char to[TO_BUF_LEN];
    safe_strncpy(to, sizeof(to), s_to_committed);
    uppercase_inplace(to);
    char for_call[FOR_BUF_LEN];
    safe_strncpy(for_call, sizeof(for_call), s_for_committed);
    uppercase_inplace(for_call);

    const nanojs8_config_t *cfg = nanojs8_config_get();
    char own[TO_BUF_LEN];
    safe_strncpy(own, sizeof(own), cfg ? cfg->callsign : "");
    uppercase_inplace(own);

    const char *text = s_text_committed;

    // 1. TO empty
    if (to[0] == '\0') return "TO callsign required";

    // 2. FOR empty (MSG TO only)
    if (msg_to && for_call[0] == '\0') return "FOR callsign required";

    // 3. TO == own (non-STORE)
    if (!store && own[0] && strcmp(to, own) == 0) {
        return "TO cannot be your own call";
    }

    // 4. MSG TO with FOR == TO
    if (msg_to && for_call[0] && strcmp(for_call, to) == 0) {
        return "FOR cannot equal TO";
    }

    // 5. QUERY MSG without numeric TEXT
    if (query_msg) {
        if (text[0] == '\0') return "MSG ID required";
        for (const char *p = text; *p; ++p) {
            if (!isdigit((unsigned char)*p)) return "MSG ID must be a number";
        }
    }

    // STORE bypasses the TX gates (no on-air activity).
    if (store) {
        if (text[0] == '\0') return "TEXT required for STORE";
        return nullptr;
    }

    // 6. No active radio profile
    if (!nanojs8_radio_get_active()) {
        return "TX OFF — configure radio";
    }

    // 7. UTC not set
    if (!nanojs8_time_is_set()) {
        return "TX OFF — set UTC in SETUP";
    }

    // FREE / MSG / MSG TO / MYLOC require TEXT (already checked in wire
    // build, but operator-facing message helps). For MYLOC this fires
    // when GPS wasn't locked at commit time so auto-fill couldn't
    // populate the field — operator either waits for fix or types
    // coords manually.
    if ((s_cmd_committed == CMD_FREE
         || s_cmd_committed == CMD_MSG
         || s_cmd_committed == CMD_MSG_TO
         || s_cmd_committed == CMD_MYLOC)
        && text[0] == '\0') {
        return "TEXT required";
    }

    return nullptr;
}

// ── Painting ─────────────────────────────────────────────────────────

void paint_chrome() {
    nanojs8_display_clear(COL_BG);
    nanojs8_display_draw_text(PAD_X, HEADER_Y, "NanoJS8",
                              COL_HEADER_NAME, COL_BG);
    const char *tag = "COMPOSE";
    int tag_w = nanojs8_display_text_width(tag);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - tag_w, HEADER_Y, tag,
                              COL_HEADER_TAG, COL_BG);
    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
}

// Helper: label color for a row given focus + mode.
uint16_t label_color_for(compose_field_t field) {
    if (s_focused != field) return COL_LABEL_NORM;
    if (s_mode == MODE_EDIT) return COL_LABEL_EDIT;
    return COL_LABEL_FOCUS;
}

// Helper: pick the buffer (committed or edit) for a text field based
// on whether the operator is currently editing it.
const char *display_text_for(compose_field_t field) {
    const bool editing = (s_mode == MODE_EDIT && s_focused == field);
    switch (field) {
        case FIELD_TO:   return editing ? s_to_edit   : s_to_committed;
        case FIELD_FOR:  return editing ? s_for_edit  : s_for_committed;
        case FIELD_TEXT: return editing ? s_text_edit : s_text_committed;
        default:         return "";
    }
}

// Paint a single text-input field row.
void paint_text_field(int y, compose_field_t field, const char *label) {
    nanojs8_display_fill_rect(0, y - 2, SCREEN_W, ROW_H + 2, COL_BG);

    nanojs8_display_draw_text(LABEL_X, y, label,
                              label_color_for(field), COL_BG);

    const char *txt = display_text_for(field);
    const bool empty = (txt[0] == '\0');
    uint16_t value_color = empty ? COL_VALUE_DIM : COL_VALUE;

    // Draw value
    int tx = VALUE_X;
    if (empty && s_focused != field) {
        nanojs8_display_draw_text(tx, y, "(empty)",
                                  value_color, COL_BG);
    } else if (empty) {
        // Focused but empty — show a caret marker on EDIT, else blank
        if (s_mode == MODE_EDIT && s_focused == field) {
            nanojs8_display_draw_text(tx, y, "|", COL_VALUE, COL_BG);
        }
    } else {
        nanojs8_display_draw_text(tx, y, txt, value_color, COL_BG);
        // Caret on the editing row
        if (s_mode == MODE_EDIT && s_focused == field) {
            int after = tx + nanojs8_display_text_width(txt);
            nanojs8_display_draw_text(after, y, "|", COL_VALUE, COL_BG);
        }
    }

    // Picker-source marker (when editing TO/FOR and the buffer matches
    // a picker entry that is a group, prefix with '*' visual marker).
    if (s_mode == MODE_EDIT
        && (field == FIELD_TO || field == FIELD_FOR)
        && s_picker_index >= 0
        && (uint32_t)s_picker_index < s_picker_count
        && s_picker_is_group[s_picker_index]) {
        // Draw '*' just before the value to mark group selection.
        nanojs8_display_draw_text(tx - FONT_W, y, "*",
                                  COL_HEADER_NAME, COL_BG);
    }
}

// L7.11f-fix1: dedicated paint path for FIELD_TEXT — multi-line
// wrap. The TEXT body can hold up to NANOJS8_TEXT_BUF_LEN-1 chars;
// at ~30 visible chars per line (≈ (SCREEN_W - VALUE_X - PAD_X)/FONT_W
// after the label column), we show up to TEXT_ROWS = 3 lines and
// append "…" if the body still overflows.
//
// Hard wrap (no word-break heuristics) keeps the implementation
// trivial and matches how the wire-form sees the buffer anyway.
constexpr int TEXT_ROWS         = 3;
constexpr int TEXT_LINE_CHARS   = 30;

void paint_text_body(int y) {
    // Reserve TEXT_ROWS * FONT_H plus a little leading for the row.
    const int row_h = TEXT_ROWS * FONT_H + 4;
    nanojs8_display_fill_rect(0, y - 2, SCREEN_W, row_h + 2, COL_BG);

    nanojs8_display_draw_text(LABEL_X, y, "TEXT",
                              label_color_for(FIELD_TEXT), COL_BG);

    const char *txt = display_text_for(FIELD_TEXT);
    const bool empty = (txt[0] == '\0');
    const bool editing = (s_mode == MODE_EDIT && s_focused == FIELD_TEXT);

    if (empty) {
        if (editing) {
            // Show a leading caret so operator knows the field is live.
            nanojs8_display_draw_text(VALUE_X, y, "|", COL_VALUE, COL_BG);
        } else {
            nanojs8_display_draw_text(VALUE_X, y, "(empty)",
                                      COL_VALUE_DIM, COL_BG);
        }
        return;
    }

    // Wrap text into TEXT_ROWS lines of TEXT_LINE_CHARS each.
    const size_t txt_len = strlen(txt);
    size_t pos           = 0;
    char line_buf[TEXT_LINE_CHARS + 2];  // +1 NUL, +1 trailing caret
    int  line            = 0;

    for (; line < TEXT_ROWS && pos < txt_len; ++line) {
        const size_t remaining = txt_len - pos;
        size_t chunk = (remaining > (size_t)TEXT_LINE_CHARS)
                         ? (size_t)TEXT_LINE_CHARS
                         : remaining;

        // If this is the LAST visible line and there's more text after,
        // truncate this line by 1 and append "…" (3 dots fit in 3 chars
        // but the … glyph is 1 char in our 8-bit font; if our font
        // doesn't render UTF-8 we'll fall back to "..." inline).
        const bool last_line_with_overflow =
            (line == TEXT_ROWS - 1) && (remaining > chunk);

        if (last_line_with_overflow && chunk >= 3) {
            chunk -= 3;
        }

        memcpy(line_buf, txt + pos, chunk);
        line_buf[chunk] = '\0';

        const int row_y = y + line * FONT_H;
        nanojs8_display_draw_text(VALUE_X, row_y, line_buf,
                                   COL_VALUE, COL_BG);

        if (last_line_with_overflow) {
            const int dots_x = VALUE_X + (int)chunk * FONT_W;
            nanojs8_display_draw_text(dots_x, row_y, "...",
                                       COL_VALUE_DIM, COL_BG);
        } else if (editing && pos + chunk >= txt_len) {
            // Caret at end of last typed char on the final wrap line.
            const int caret_x = VALUE_X + (int)chunk * FONT_W;
            nanojs8_display_draw_text(caret_x, row_y, "|",
                                       COL_VALUE, COL_BG);
        }

        pos += chunk;
    }
}

// Paint the CMD field row (no buffer; just shows the verb).
void paint_cmd_field(int y) {
    nanojs8_display_fill_rect(0, y - 2, SCREEN_W, ROW_H + 2, COL_BG);
    nanojs8_display_draw_text(LABEL_X, y, "CMD",
                              label_color_for(FIELD_CMD), COL_BG);

    const compose_cmd_t cmd_shown =
        (s_mode == MODE_EDIT && s_focused == FIELD_CMD)
            ? s_cmd_edit
            : s_cmd_committed;
    const char *label = CMD_LABEL[cmd_shown];

    int tx = VALUE_X;
    // Up/down arrows hint when CMD is being edited
    if (s_mode == MODE_EDIT && s_focused == FIELD_CMD) {
        nanojs8_display_draw_text(tx, y, "<", COL_HEADER_TAG, COL_BG);
        tx += FONT_W + 2;
        nanojs8_display_draw_text(tx, y, label, COL_VALUE, COL_BG);
        tx += nanojs8_display_text_width(label) + 4;
        nanojs8_display_draw_text(tx, y, ">", COL_HEADER_TAG, COL_BG);
    } else {
        nanojs8_display_draw_text(tx, y, label, COL_VALUE, COL_BG);
    }
}

// Paint the SEND button row.
void paint_send_button(int y) {
    nanojs8_display_fill_rect(0, y - 2, SCREEN_W, ROW_H + 2, COL_BG);

    const bool focused = (s_focused == FIELD_SEND);
    const char *label = (s_cmd_committed == CMD_STORE) ? "STORE" : "SEND";
    int btn_w = 80;
    int btn_h = ROW_H - 4;
    int btn_x0 = (SCREEN_W - btn_w) / 2;
    int btn_y0 = y;

    uint16_t bg = focused ? COL_ACCENT_BG : COL_BG;
    uint16_t fg = focused ? COL_FOCUS_FG  : COL_VALUE;
    nanojs8_display_fill_rect(btn_x0, btn_y0, btn_w, btn_h, bg);
    // Border for the button (1px outline by overdraw)
    nanojs8_display_fill_rect(btn_x0, btn_y0, btn_w, 1, COL_SEPARATOR);
    nanojs8_display_fill_rect(btn_x0, btn_y0 + btn_h - 1, btn_w, 1,
                              COL_SEPARATOR);
    nanojs8_display_fill_rect(btn_x0, btn_y0, 1, btn_h, COL_SEPARATOR);
    nanojs8_display_fill_rect(btn_x0 + btn_w - 1, btn_y0, 1, btn_h,
                              COL_SEPARATOR);

    int label_w = nanojs8_display_text_width(label);
    int label_x = btn_x0 + (btn_w - label_w) / 2;
    int label_y = btn_y0 + (btn_h - FONT_H) / 2 + 1;
    nanojs8_display_draw_text(label_x, label_y, label, fg, bg);
}

// Footer with contextual hint + TX warning on the right.
void paint_footer() {
    nanojs8_display_fill_rect(0, FOOTER_Y - 2, SCREEN_W,
                              FONT_H + 4, COL_BG);

    // L7.14-fix3: transient error banner takes precedence over the
    // contextual hint. 4-second TTL via esp_timer; after the banner
    // expires we fall through to the normal hint band. Color is
    // bright red so the operator catches the failure at-a-glance.
    const int64_t now_us = esp_timer_get_time();
    if (s_error_until_us > 0 && now_us < s_error_until_us
        && s_error_msg[0] != '\0') {
        nanojs8_display_draw_text(PAD_X, FOOTER_Y, s_error_msg,
                                  NANOJS8_COLOR_RED, COL_BG);
        return;
    }

    const char *hint;
    if (s_mode == MODE_EDIT) {
        switch (s_focused) {
            case FIELD_TO:
            case FIELD_FOR:
                // L7.11f-fix1: drop confusing "ESC cx" — use
                // backspace-on-empty as the cancel gesture instead.
                hint = s_picker_count > 0
                         ? "type or UP/DN list  ENT commit"
                         : "type  ENT commit";
                break;
            case FIELD_CMD:
                hint = "UP/DN verb  ENT commit";
                break;
            case FIELD_TEXT:
                hint = "type  ENT commit  BKSP-empty cancels";
                break;
            default: hint = "ENT commit"; break;
        }
    } else {
        hint = "UP/DN field  ENT edit/send  L/R cycle";
    }
    nanojs8_display_draw_text(PAD_X, FOOTER_Y, hint,
                              COL_FOOTER, COL_BG);

    // Right-side TX warning (NAV only — too noisy in EDIT)
    if (s_mode == MODE_NAV) {
        const char *warn = compute_tx_warning();
        if (warn) {
            int ww = nanojs8_display_text_width(warn);
            // Make sure it doesn't overdraw hint; if it would, skip
            int hint_w = nanojs8_display_text_width(hint);
            if (PAD_X + hint_w + 8 + ww + PAD_X <= SCREEN_W) {
                nanojs8_display_draw_text(SCREEN_W - PAD_X - ww,
                                           FOOTER_Y, warn, COL_WARN,
                                           COL_BG);
            }
        }
    }
}

void render(bool full_redraw) {
    if (full_redraw || s_last_rendered_seq != s_dirty_seq) {
        paint_chrome();

        // Rows
        int y = FIELD_Y0;
        paint_text_field(y, FIELD_TO, "TO");
        y += ROW_H;
        paint_cmd_field(y);
        y += ROW_H;
        if (is_msg_to()) {
            paint_text_field(y, FIELD_FOR, "FOR");
            y += ROW_H;
        }
        // L7.11f-fix1: TEXT row uses the dedicated wrapping paint
        // path and reserves room for TEXT_ROWS visual lines.
        paint_text_body(y);
        y += TEXT_ROWS * FONT_H + 8;
        paint_send_button(y);

        paint_footer();
        s_last_rendered_seq = s_dirty_seq;
    }
}

void on_enter() {
    s_left_ticks = s_right_ticks = s_up_ticks = s_down_ticks = 0;
    s_mode       = MODE_NAV;
    s_focused    = FIELD_TO;
    s_picker_index = -1;
    s_last_rendered_seq = 0xFFFFFFFFu;  // force repaint
    mark_dirty();
    ESP_LOGI(TAG, "Entering COMPOSE (CMD=%s)", CMD_LABEL[s_cmd_committed]);
}

// ── Commit / cancel / fire ───────────────────────────────────────────

void enter_edit_mode() {
    if (s_mode == MODE_EDIT) return;
    s_mode = MODE_EDIT;
    s_picker_index = -1;
    switch (s_focused) {
        case FIELD_TO:
            safe_strncpy(s_to_edit, sizeof(s_to_edit), s_to_committed);
            rebuild_picker_list();
            break;
        case FIELD_FOR:
            safe_strncpy(s_for_edit, sizeof(s_for_edit), s_for_committed);
            rebuild_picker_list();
            break;
        case FIELD_TEXT:
            safe_strncpy(s_text_edit, sizeof(s_text_edit),
                         s_text_committed);
            break;
        case FIELD_CMD:
            s_cmd_edit = s_cmd_committed;
            break;
        default: break;
    }
    mark_dirty();
}

void commit_edit() {
    if (s_mode != MODE_EDIT) return;
    switch (s_focused) {
        case FIELD_TO:
            uppercase_inplace(s_to_edit);
            safe_strncpy(s_to_committed, sizeof(s_to_committed),
                         s_to_edit);
            break;
        case FIELD_FOR:
            uppercase_inplace(s_for_edit);
            safe_strncpy(s_for_committed, sizeof(s_for_committed),
                         s_for_edit);
            break;
        case FIELD_TEXT:
            safe_strncpy(s_text_committed, sizeof(s_text_committed),
                         s_text_edit);
            break;
        case FIELD_CMD: {
            // If switching to/from MSG_TO, snap focus to a sensible row
            const bool was_msg_to = (s_cmd_committed == CMD_MSG_TO);
            const compose_cmd_t prev_cmd = s_cmd_committed;
            s_cmd_committed = s_cmd_edit;

            // L7.14-fix6: when operator commits MYLOC for the first
            // time (transitioning from any other verb) AND the TEXT
            // field is empty, auto-populate TEXT with the current GPS
            // coords. Fill-once policy: if operator already typed
            // something we don't overwrite. If GPS has no valid fix
            // yet we banner an error and leave TEXT empty — operator
            // can either type coords manually or wait for fix.
            if (s_cmd_committed == CMD_MYLOC && prev_cmd != CMD_MYLOC) {
                if (s_text_committed[0] == '\0') {
                    char coords[40];
                    if (nanojs8_gps_format_position(coords, sizeof(coords))) {
                        safe_strncpy(s_text_committed, sizeof(s_text_committed),
                                     coords);
                        safe_strncpy(s_text_edit, sizeof(s_text_edit),
                                     coords);
                        ESP_LOGI(TAG,
                            "MYLOC: auto-filled TEXT with GPS coords '%s'",
                            coords);
                    } else {
                        set_error("GPS not locked — try later");
                    }
                }
            }

            const bool now_msg_to = (s_cmd_committed == CMD_MSG_TO);
            if (was_msg_to && !now_msg_to && s_focused == FIELD_FOR) {
                s_focused = FIELD_CMD;
            }
            break;
        }
        default: break;
    }
    s_mode = MODE_NAV;
    s_picker_index = -1;
    mark_dirty();
}

void cancel_edit() {
    if (s_mode != MODE_EDIT) return;
    // Discard the edit buffer (no copy back to committed)
    s_mode = MODE_NAV;
    s_picker_index = -1;
    mark_dirty();
}

// L7.11g.3-fix2: Clear all editable buffers after a successful SEND
// or STORE so the operator perceives the action completed — empty
// fields are an unambiguous "yes, that went through" signal. Without
// this, the previous TO/FREE-TEXT/FOR persist on screen and the
// operator can't tell whether they pressed SEND or whether it
// "stuck". CMD is preserved so the operator can queue another send
// of the same verb without re-selecting; focus snaps back to TO so
// the next input lands where they're probably about to retype.
void reset_compose_fields_after_send() {
    s_to_committed[0]   = '\0';
    s_to_edit[0]        = '\0';
    s_for_committed[0]  = '\0';
    s_for_edit[0]       = '\0';
    s_text_committed[0] = '\0';
    s_text_edit[0]      = '\0';
    s_focused           = FIELD_TO;
    mark_dirty();
}

// Fire the SEND button — depending on CMD, either TX or STORE.
void fire_send() {
    if (nanojs8_tx_audio_is_active() && s_cmd_committed != CMD_STORE) {
        ESP_LOGW(TAG, "SEND ignored — TX already in flight");
        set_error("TX already in flight — wait");
        return;
    }

    const char *warn = compute_tx_warning();
    if (warn) {
        ESP_LOGW(TAG, "SEND refused: %s", warn);
        set_error(warn);   // operator-friendly strings already
        return;
    }

    if (s_cmd_committed == CMD_STORE) {
        // L7.11g.2: API changed from store(to, body) to
        // add_store(from, to, body). For a COMPOSE→STORE the
        // originator IS us — the operator is asking the mailbox to
        // hold a message addressed to s_to_committed until that
        // callsign queries us via QUERY MSGS (L7.11g.6 handler).
        const nanojs8_config_t *cfg = nanojs8_config_get();
        esp_err_t err = nanojs8_mailbox_add_store(
                                cfg ? cfg->callsign : "",
                                s_to_committed,
                                s_text_committed);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "STORE: msg for %s saved to mailbox "
                          "(%u entries held)",
                     s_to_committed,
                     (unsigned)nanojs8_mailbox_count());
            // L7.11g.3-fix2: clear ALL fields, not just TEXT. Empty
            // form = visible confirmation the STORE succeeded.
            reset_compose_fields_after_send();
            set_error(nullptr);   // clear any stale banner
        } else {
            ESP_LOGE(TAG, "STORE: mailbox_add_store returned 0x%x",
                     (int)err);
            // Mailbox is usually full when this fires (NVS blob
            // capacity = 16 entries) — operator action is to
            // delete old messages from INBOX.
            set_error("Mailbox full — clear INBOX entries");
        }
        return;
    }

    // TX path: build wire, queue.
    char wire[200];
    if (!build_wire(wire, sizeof(wire))) {
        ESP_LOGE(TAG, "SEND: build_wire failed — refusing");
        set_error("Message build failed — check fields");
        return;
    }
    ESP_LOGI(TAG, "SEND wire: '%s'", wire);
    esp_err_t err = nanojs8_tx_audio_transmit_text(wire);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SEND: transmit_text failed 0x%x", (int)err);
        // L7.14-fix9: surface specific failure modes with operator-
        // friendly banners instead of the generic "TX failed (0x..)".
        // INVALID_SIZE = message packs to too many JS8 frames (>8).
        // INVALID_ARG  = couldn't pack at all (very unusual; bad
        //                chars or wire structure).
        // Everything else (INVALID_STATE etc) falls through to the
        // hex error code — those are programming-error conditions
        // the operator can't directly fix.
        if (err == ESP_ERR_INVALID_SIZE) {
            set_error("Message too long — shorten and try again");
        } else if (err == ESP_ERR_NO_MEM) {
            // L7.14-fix10: PSRAM insufficient for the frame caches
            // this message would need. Same operator action as
            // INVALID_SIZE — shorten the body — so we reuse the
            // banner. Underlying difference is logged.
            set_error("Message too long — shorten and try again");
        } else if (err == ESP_ERR_INVALID_ARG) {
            set_error("Message couldn't be packed — check fields");
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "TX failed (0x%x)", (int)err);
            set_error(buf);
        }
        return;
    }

    // TX accepted by the queue — clear any stale error banner left
    // over from a prior failed attempt so it doesn't linger past
    // the success.
    set_error(nullptr);

    // L7.11f-fix2c: log the outgoing message to DIRECTED so the
    // operator sees their own SEND interleaved with received traffic.
    // Tokenize wire as "to_call verb body" — same parse MicroJS8 uses
    // (app.py _record_directed_out_from_text): split on whitespace into
    // at most 3 tokens. Anything past the second whitespace is body.
    // For wires that don't fit the directed form (rare from COMPOSE
    // since the FROM/TO/CMD fields shape the wire) we degrade
    // gracefully: verb=first token, body="".
    {
        char to_call[NANOJS8_ACTIVITY_CALL_MAX] = {0};
        char verb[NANOJS8_ACTIVITY_VERB_MAX]    = {0};
        char body[NANOJS8_ACTIVITY_BODY_MAX]    = {0};

        const char *p = wire;
        while (*p == ' ') ++p;
        // Token 1 → to_call
        size_t i = 0;
        while (*p && *p != ' ' && i + 1 < sizeof(to_call)) {
            to_call[i++] = *p++;
        }
        to_call[i] = '\0';
        while (*p == ' ') ++p;
        // Token 2 → verb
        i = 0;
        while (*p && *p != ' ' && i + 1 < sizeof(verb)) {
            verb[i++] = *p++;
        }
        verb[i] = '\0';
        while (*p == ' ') ++p;
        // Remainder → body (whole rest of wire, may include spaces)
        i = 0;
        while (*p && i + 1 < sizeof(body)) {
            body[i++] = *p++;
        }
        body[i] = '\0';

        // Fallback: if only one token, treat it as verb (matches
        // MicroJS8 graceful path).
        if (verb[0] == '\0' && to_call[0] != '\0') {
            strncpy(verb, to_call, sizeof(verb) - 1);
            verb[sizeof(verb) - 1] = '\0';
            to_call[0] = '\0';
        }

        nanojs8_activity_record_out(to_call, verb, body);
    }

    // L7.11f-fix1: redirect to DIRECTED so the operator sees their
    // sent message and any incoming responses (matches MicroJS8's
    // post-SEND UX). STORE doesn't redirect — operator may want to
    // queue multiple messages back-to-back on COMPOSE.
    //
    // L7.11g.3-fix2: clear the form fields before redirecting. When
    // the operator cycles back to COMPOSE later, they see an empty
    // form — unambiguous confirmation the send went through, and
    // they're not at risk of double-sending the same content by
    // accident if they hit Enter again on a stale-looking screen.
    reset_compose_fields_after_send();
    ESP_LOGI(TAG, "SEND queued — auto-switching to DIRECTED");
    nanojs8_ui_set_screen(NANOJS8_SCREEN_DIRECTED);
}

// ── Input handling ───────────────────────────────────────────────────

bool handle_input_nav(uint8_t event) {
    // Ring navigation
    if (event == NANOJS8_TRACKBALL_LEFT) {
        s_right_ticks = s_up_ticks = s_down_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            // L7.11g.3: INBOX inserted between DIRECTED and COMPOSE,
            // so LEFT from COMPOSE now lands on INBOX, not DIRECTED.
            nanojs8_ui_set_screen(NANOJS8_SCREEN_INBOX);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        s_left_ticks = s_up_ticks = s_down_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_ALLCALL);
        }
        return true;
    }

    // Field cycling
    if (event == NANOJS8_TRACKBALL_UP) {
        s_down_ticks = s_left_ticks = s_right_ticks = 0;
        if (++s_up_ticks >= FIELD_TICK_THRESHOLD) {
            s_up_ticks = 0;
            s_focused = next_field(s_focused, /*forward=*/false);
            ESP_LOGI(TAG, "NAV focus -> %d", (int)s_focused);
            mark_dirty();
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_DOWN) {
        s_up_ticks = s_left_ticks = s_right_ticks = 0;
        if (++s_down_ticks >= FIELD_TICK_THRESHOLD) {
            s_down_ticks = 0;
            s_focused = next_field(s_focused, /*forward=*/true);
            ESP_LOGI(TAG, "NAV focus -> %d", (int)s_focused);
            mark_dirty();
        }
        return true;
    }

    // Activate focused field
    if (event == NANOJS8_TRACKBALL_CLICK || event == '\r') {
        if (s_focused == FIELD_SEND) {
            fire_send();
        } else {
            enter_edit_mode();
        }
        return true;
    }

    // Anything else in NAV mode: ignore (don't accidentally type into
    // a field; operator must enter EDIT mode first).
    return false;
}

bool handle_input_edit(uint8_t event) {
    // ESC always cancels.
    if (event == 0x1B) {
        cancel_edit();
        return true;
    }

    // Enter or CLICK commits.
    if (event == '\r' || event == NANOJS8_TRACKBALL_CLICK) {
        commit_edit();
        return true;
    }

    // Per-field UP/DOWN behavior
    if (event == NANOJS8_TRACKBALL_UP || event == NANOJS8_TRACKBALL_DOWN) {
        const bool forward = (event == NANOJS8_TRACKBALL_DOWN);
        switch (s_focused) {
            case FIELD_CMD: {
                // Cycle verb (with tick-debounce to avoid skipping).
                if (forward) {
                    if (++s_down_ticks >= PICKER_TICK_THRESHOLD) {
                        s_down_ticks = 0;
                        s_cmd_edit = (compose_cmd_t)
                            (((int)s_cmd_edit + 1) % CMD_COUNT);
                        mark_dirty();
                    }
                } else {
                    if (++s_up_ticks >= PICKER_TICK_THRESHOLD) {
                        s_up_ticks = 0;
                        s_cmd_edit = (compose_cmd_t)
                            (((int)s_cmd_edit + CMD_COUNT - 1) % CMD_COUNT);
                        mark_dirty();
                    }
                }
                return true;
            }
            case FIELD_TO:
            case FIELD_FOR: {
                char *buf = (s_focused == FIELD_TO) ? s_to_edit : s_for_edit;
                size_t buf_n = (s_focused == FIELD_TO)
                                 ? sizeof(s_to_edit)
                                 : sizeof(s_for_edit);
                if (forward) {
                    if (++s_down_ticks >= PICKER_TICK_THRESHOLD) {
                        s_down_ticks = 0;
                        picker_cycle(true, buf, buf_n);
                    }
                } else {
                    if (++s_up_ticks >= PICKER_TICK_THRESHOLD) {
                        s_up_ticks = 0;
                        picker_cycle(false, buf, buf_n);
                    }
                }
                return true;
            }
            case FIELD_TEXT:
            default:
                // No useful UP/DOWN action in TEXT EDIT.
                return true;
        }
    }

    // LEFT/RIGHT trackball: ignored in EDIT mode (operator must commit
    // or cancel first). Matches SETUP behavior.
    if (event == NANOJS8_TRACKBALL_LEFT
        || event == NANOJS8_TRACKBALL_RIGHT) {
        return true;
    }

    // Backspace
    if (event == 0x08) {
        char *buf = nullptr;
        switch (s_focused) {
            case FIELD_TO:   buf = s_to_edit;   break;
            case FIELD_FOR:  buf = s_for_edit;  break;
            case FIELD_TEXT: buf = s_text_edit; break;
            default: return true;
        }
        size_t len = strlen(buf);
        if (len > 0) {
            buf[len - 1] = '\0';
            picker_reset();
            mark_dirty();
        } else {
            // L7.11f-fix1: BACKSPACE on an already-empty buffer is
            // the cancel-edit gesture, since T-Deck Plus keyboards
            // typically don't emit a real ESC (0x1B). Operator
            // workflow: keep pressing BACKSPACE; once the field is
            // empty, the next press bails out without committing
            // the empty value.
            ESP_LOGI(TAG, "EDIT cancel via BACKSPACE-on-empty");
            cancel_edit();
        }
        return true;
    }

    // Printable chars — append to the focused buffer.
    if (event >= 0x20 && event <= 0x7E) {
        char *buf = nullptr;
        size_t buf_n = 0;
        switch (s_focused) {
            case FIELD_TO:
                buf = s_to_edit;   buf_n = sizeof(s_to_edit);  break;
            case FIELD_FOR:
                buf = s_for_edit;  buf_n = sizeof(s_for_edit); break;
            case FIELD_TEXT:
                buf = s_text_edit; buf_n = sizeof(s_text_edit); break;
            default:
                return true;
        }
        size_t len = strlen(buf);
        if (len + 1 < buf_n) {
            // L7.11f-fix1: auto-uppercase ALL editable fields, not
            // just TO/FOR. JS8 protocol's wire encoding (Varicode)
            // is case-insensitive and normalizes lowercase to
            // uppercase before transmission anyway — doing it here
            // gives the operator visual feedback that matches the
            // on-air payload, and helps the multi-frame data path
            // pack more efficiently (uppercase chars have shorter
            // Varicode codepoints in some positions).
            char c = (char)event;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            buf[len]     = c;
            buf[len + 1] = '\0';
            picker_reset();
            mark_dirty();
        }
        return true;
    }

    return false;
}

bool handle_input(uint8_t event) {
    if (s_mode == MODE_EDIT) {
        return handle_input_edit(event);
    }
    return handle_input_nav(event);
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_COMPOSE = {
    .id           = NANOJS8_SCREEN_COMPOSE,
    .name         = "COMPOSE",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
