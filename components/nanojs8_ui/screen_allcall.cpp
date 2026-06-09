/*
 * screen_allcall.cpp — L7.11e ALLCALL TX menu
 * ============================================
 * Mirrors MicroJS8's ALLCALL screen (src/microjs8/ui/screens.py
 * _render_allcall + input/router.py _handle_allcall_key). Three rows:
 *
 *   ┌────────────────────────────────────────┐
 *   │ NanoJS8                       ALLCALL  │
 *   ├────────────────────────────────────────┤
 *   │                                        │
 *   │   HEARTBEAT                    SEND    │  ← focused row
 *   │                                        │
 *   │   QUERY MSGS                   SEND    │
 *   │                                        │
 *   │   CQ                           SEND    │
 *   │                                        │
 *   ├────────────────────────────────────────┤
 *   │ UP/DOWN pick  ENTER send  L/R cycle    │
 *   └────────────────────────────────────────┘
 *
 * UI behavior:
 *   - Trackball UP / DOWN cycles the focus row (with the same
 *     sensitivity-tick threshold as other screens — operators don't
 *     accidentally skip rows from a brushed trackball).
 *   - ASCII Enter (0x0D from the C3 keyboard) AND trackball CLICK
 *     (0x86) both invoke the focused row.
 *   - Trackball LEFT cycles to DIRECTED (previous on ring).
 *   - Trackball RIGHT cycles to HOME (closes the ring).
 *
 * Wire-format per row (the from-envelope "<call>: " is added by the
 * encoder, never typed here):
 *
 *   row 0 (HEARTBEAT)   "@HB HEARTBEAT <grid4>"  or  "@HB HEARTBEAT"
 *                       if grid is empty (legal heartbeat form).
 *   row 1 (QUERY MSGS)  "@ALLCALL QUERY MSGS"
 *   row 2 (CQ)          "CQ CQ CQ <grid4>"
 *                       Refuses if grid not configured — a CQ without
 *                       a locator is meaningless to receiving stations.
 *
 * All three transmissions are single-shot: they fire once on Enter,
 * and the operator must press Enter again to repeat. Scheduled-cadence
 * heartbeats (SINGLE / 20 MIN / 1 HR like MicroJS8 HbMode) are
 * deferred to L7.11h.
 *
 * License: GPL-3.0
 */

#include "ui_internal.h"
#include "display.h"
#include "trackball.h"
#include "config.h"
#include "activity.h"          // L7.11f-fix2c: record_out for OUT log
#include "tx_audio.h"
#include "time_source.h"       // L7.13-fix3: nanojs8_time_is_set() for pre-check
#include "radio.h"             // L7.13-fix3: nanojs8_radio_get_active() for pre-check

#include "esp_log.h"
#include "esp_timer.h"         // L7.13-fix3: monotonic clock for transient error banner

#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "screen_allcall";

// ── Layout ───────────────────────────────────────────────────────────
constexpr int SCREEN_W   = NANOJS8_DISPLAY_WIDTH;
constexpr int SCREEN_H   = NANOJS8_DISPLAY_HEIGHT;
constexpr int FONT_H     = 16;
constexpr int FONT_W     = 8;
constexpr int PAD_X      = 6;

constexpr int HEADER_Y    = 6;
constexpr int SEPARATOR_Y = HEADER_Y + FONT_H + 4;

// Three rows of fixed height. Generous 32 px so the highlight band
// reads cleanly and the focus state is unambiguous at arm's length.
constexpr int ROW_H       = 32;
constexpr int ROWS_TOP    = SEPARATOR_Y + 14;
constexpr int FOOTER_Y    = SCREEN_H - FONT_H - 6;

constexpr int ROWS_COUNT  = 3;  // HEARTBEAT / QUERY MSGS / CQ

// ── Colors ───────────────────────────────────────────────────────────
constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;
constexpr uint16_t COL_HEADER_TAG  = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_LABEL       = NANOJS8_COLOR_WHITE;
constexpr uint16_t COL_LABEL_DIM   = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_FOCUS_FG    = NANOJS8_COLOR_BLACK;  // on accent bg
constexpr uint16_t COL_ACCENT_BG   = NANOJS8_COLOR_CYAN;
constexpr uint16_t COL_FOOTER      = NANOJS8_COLOR_GRAY;
constexpr uint16_t COL_HINT_DIM    = NANOJS8_COLOR_DARK_GRAY;

// Sensitivity dividers (mirror screen_setup.cpp / screen_directed.cpp).
constexpr int SWITCH_TICK_THRESHOLD = 4;  // screen-cycle stiffness
constexpr int FOCUS_TICK_THRESHOLD  = 2;  // row-cycle stiffness — softer
                                          // because rows are intentional
                                          // and operators want them to
                                          // feel responsive (only 3 rows
                                          // total).

// ── Module state ─────────────────────────────────────────────────────
int s_focus_row    = 0;     // 0=HEARTBEAT  1=QUERY MSGS  2=CQ

int s_left_ticks   = 0;
int s_right_ticks  = 0;
int s_up_ticks     = 0;
int s_down_ticks   = 0;

// Dirty detection so we only repaint on actual change. The on_enter
// callback resets these and forces a paint.
int s_last_focus_drawn = -1;

// L7.13-fix3: transient error banner state. When transmit_text fails
// for any reason, we stash a short human-readable message here with an
// expiry timestamp; render() draws it in the footer band instead of
// the static hotkey hint. Once esp_timer passes the expiry, the next
// render() reverts to the normal footer. Operator no longer needs to
// have a serial console attached to discover why a TX failed.
constexpr int64_t ERROR_BANNER_US = 4 * 1000 * 1000;  // 4 seconds on screen
char    s_error_msg[64]   = {0};
int64_t s_error_until_us  = 0;     // 0 = no error active

// ── Row table ────────────────────────────────────────────────────────
//
// The labels are also the wire-form verb in two of three cases —
// "HEARTBEAT" maps to "@HB HEARTBEAT", "CQ" to "CQ CQ CQ" — but we
// keep the table simple by carrying labels here and building the wire
// inline in invoke_row(). Keeps the menu data exactly as it appears
// on screen.
const char * const ROW_LABEL[ROWS_COUNT] = {
    "HEARTBEAT",
    "QUERY MSGS",
    "CQ",
};

// ── Painting ─────────────────────────────────────────────────────────

void paint_chrome() {
    nanojs8_display_clear(COL_BG);

    nanojs8_display_draw_text(PAD_X, HEADER_Y, "NanoJS8",
                              COL_HEADER_NAME, COL_BG);
    const char *tag = "ALLCALL";
    int tag_w = nanojs8_display_text_width(tag);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - tag_w, HEADER_Y, tag,
                              COL_HEADER_TAG, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
}

void paint_rows(int focus) {
    // Each row: a 32-px tall band. Focused row gets the accent-bg
    // fill so the operator's eye lands on it instantly. SEND on the
    // right side dims on unfocused rows and lights up on the focused
    // one — gives a "press Enter to send THIS" affordance without
    // needing prose.
    for (int i = 0; i < ROWS_COUNT; ++i) {
        const int row_y = ROWS_TOP + i * ROW_H;
        const bool focused = (i == focus);

        // Fill the row's whole band so any previous focus state's
        // accent paint gets erased.
        const uint16_t bg = focused ? COL_ACCENT_BG : COL_BG;
        nanojs8_display_fill_rect(0, row_y, SCREEN_W, ROW_H, bg);

        const uint16_t label_color = focused ? COL_FOCUS_FG : COL_LABEL;
        const uint16_t send_color  =
            focused ? COL_FOCUS_FG : COL_HINT_DIM;

        // Label on the left, vertical-center the text within the band
        // (band is ROW_H tall, font is FONT_H tall; offset (ROW_H-FONT_H)/2).
        const int text_y = row_y + (ROW_H - FONT_H) / 2;
        nanojs8_display_draw_text(PAD_X + 4, text_y, ROW_LABEL[i],
                                   label_color, bg);

        // "SEND" on the right.
        const char *send = "SEND";
        const int send_w = nanojs8_display_text_width(send);
        nanojs8_display_draw_text(SCREEN_W - PAD_X - 4 - send_w,
                                   text_y, send, send_color, bg);
    }
}

void paint_footer() {
    nanojs8_display_fill_rect(0, FOOTER_Y - 2, SCREEN_W,
                              FONT_H + 4, COL_BG);

    // L7.13-fix3: if an error banner is active (set by invoke_row's
    // pre-check failure) AND not yet expired, render it instead of the
    // static hotkey hint. Color is RED for "something failed" — at-a-
    // glance distinct from the dim-gray hint, so operator sees the
    // failure without needing serial.
    const int64_t now_us = esp_timer_get_time();
    if (s_error_until_us > 0 && now_us < s_error_until_us
        && s_error_msg[0] != '\0') {
        nanojs8_display_draw_text(PAD_X, FOOTER_Y, s_error_msg,
                                  NANOJS8_COLOR_RED, COL_BG);
        return;
    }

    nanojs8_display_draw_text(PAD_X, FOOTER_Y,
                              "UP/DOWN pick  ENTER send  L/R cycle",
                              COL_FOOTER, COL_BG);
}

// L7.13-fix3: stash an error message for paint_footer to render on
// the next paint. Caller is responsible for forcing the re-render
// (typically via s_last_focus_drawn = -1; see invoke_row).
void set_error(const char *msg) {
    if (!msg) {
        s_error_msg[0]   = '\0';
        s_error_until_us = 0;
        return;
    }
    strncpy(s_error_msg, msg, sizeof(s_error_msg) - 1);
    s_error_msg[sizeof(s_error_msg) - 1] = '\0';
    s_error_until_us = esp_timer_get_time() + ERROR_BANNER_US;
}

void render(bool full_redraw) {
    // The ui dispatcher passes `full_redraw=true` when the screen has
    // just been entered or when the chrome may need repainting (e.g.
    // after a SETUP edit changed the header band). On true, drop the
    // dirty-detect cache so the next paint repaints chrome, rows, and
    // footer — same effect as the initial on_enter paint.
    if (full_redraw) {
        s_last_focus_drawn = -1;
    }

    if (s_last_focus_drawn != s_focus_row) {
        // First-time paint (last == -1) gets full chrome; subsequent
        // focus changes just repaint the rows — chrome and footer are
        // static so we save the work.
        if (s_last_focus_drawn < 0) {
            paint_chrome();
            paint_footer();
        }
        paint_rows(s_focus_row);
        s_last_focus_drawn = s_focus_row;
    }

    // L7.13-fix3: footer band repaint for the transient error banner.
    // Two transitions need handling without disturbing the rows above:
    //
    //   (a) banner just appeared    — s_last_footer_banner was false
    //                                  but s_error_until_us is in the
    //                                  future → repaint footer
    //   (b) banner just expired     — s_last_footer_banner was true
    //                                  but expiry has passed → repaint
    //                                  footer (reverts to hotkey hint)
    //
    // We track only a single bool of "was the banner on last frame?"
    // because render() is polled frequently — by the time the UI loop
    // ticks, the banner state is either current-on or current-off, no
    // intermediate state to worry about.
    static bool s_last_footer_banner = false;
    const bool  banner_now =
        (s_error_until_us > 0 &&
         esp_timer_get_time() < s_error_until_us &&
         s_error_msg[0] != '\0');
    if (banner_now != s_last_footer_banner) {
        paint_footer();
        s_last_footer_banner = banner_now;
    }
}

void on_enter() {
    // Reset sensitivity ticks so we don't carry over partial swipes
    // from a previous screen.
    s_left_ticks  = 0;
    s_right_ticks = 0;
    s_up_ticks    = 0;
    s_down_ticks  = 0;
    // Force a full repaint.
    s_last_focus_drawn = -1;

    ESP_LOGI(TAG, "Entering ALLCALL (focus=%d %s)",
             s_focus_row, ROW_LABEL[s_focus_row]);
}

// ── Row invocation ───────────────────────────────────────────────────
//
// Build the wire string for the given row and fire it through
// nanojs8_tx_audio_transmit_text(). All gates (UTC set, active radio
// profile, callsign configured, single-instance lock) are enforced
// inside transmit_text — we only need to handle row-specific gates
// (CQ needing a grid) here.

void invoke_row(int row) {
    // L7.13-fix3: clear any stale error banner before starting a fresh
    // attempt — if THIS press succeeds, the operator shouldn't be
    // staring at the previous failure message. paint_footer reverts to
    // the static hotkey hint on the next render once s_error_until_us
    // is back at 0.
    set_error(nullptr);

    if (nanojs8_tx_audio_is_active()) {
        ESP_LOGW(TAG, "Row %d (%s) invoke ignored — TX already in flight",
                 row, ROW_LABEL[row]);
        set_error("TX already in flight — wait for current");
        s_last_focus_drawn = -1;     // force footer repaint via render()
        return;
    }

    // L7.13-fix3: precondition pre-checks. transmit_text() does its own
    // pass at each of these, but it returns the same ESP_ERR_INVALID_STATE
    // for every distinct failure (init, UTC, radio, callsign, active),
    // which made the prior "check UTC + radio + callsign" message
    // actively misleading when the real problem was that the boot-time
    // self-test never ran. By checking explicitly here in order, we can
    // give the operator a specific message — both in the log and on
    // screen — for which precondition failed.
    if (!nanojs8_tx_audio_is_initialized()) {
        ESP_LOGE(TAG,
            "Row %d (%s): TX audio subsystem not initialized — boot-time "
            "self-test must have failed (intermittent heap fragmentation; "
            "fixed in L7.13-fix3 by moving the self-test stack to PSRAM). "
            "Reboot to retry.",
            row, ROW_LABEL[row]);
        set_error("TX subsystem not ready — reboot device");
        s_last_focus_drawn = -1;
        return;
    }
    if (!nanojs8_time_is_set()) {
        ESP_LOGE(TAG,
            "Row %d (%s): UTC not set — JS8 slot alignment unavailable. "
            "Enter UTC via SETUP row 6 first.",
            row, ROW_LABEL[row]);
        set_error("Set UTC in SETUP row 6 first");
        s_last_focus_drawn = -1;
        return;
    }
    if (nanojs8_radio_get_active() == nullptr) {
        ESP_LOGE(TAG,
            "Row %d (%s): no active radio profile — pick one in SETUP "
            "row 5.",
            row, ROW_LABEL[row]);
        set_error("Pick radio profile in SETUP row 5");
        s_last_focus_drawn = -1;
        return;
    }

    const nanojs8_config_t *cfg = nanojs8_config_get();
    if (!cfg || cfg->callsign[0] == '\0') {
        ESP_LOGE(TAG,
            "Row %d (%s): callsign not configured — set it in SETUP "
            "row 1.",
            row, ROW_LABEL[row]);
        set_error("Set callsign in SETUP row 1");
        s_last_focus_drawn = -1;
        return;
    }

    const bool has_grid = (cfg->grid[0] != '\0');

    char wire[40];
    switch (row) {
        case 0:  // HEARTBEAT
            // Heartbeat form tolerates a missing grid; the encoder
            // packs as a basic @HB HEARTBEAT in that case.
            if (has_grid) {
                snprintf(wire, sizeof(wire),
                         "@HB HEARTBEAT %.4s", cfg->grid);
            } else {
                snprintf(wire, sizeof(wire), "@HB HEARTBEAT");
            }
            break;
        case 1:  // QUERY MSGS
            // No grid required — just a broadcast directed at @ALLCALL.
            // Replies land in DIRECTED via the normal decode path.
            snprintf(wire, sizeof(wire), "@ALLCALL QUERY MSGS");
            break;
        case 2:  // CQ
            // CQ without a grid is meaningless to receiving stations
            // (they'd have no idea where you are). Refuse rather than
            // send a half-form.
            if (!has_grid) {
                ESP_LOGE(TAG, "Row 2 (CQ) refused: no grid configured "
                              "— enter your grid in SETUP first");
                set_error("CQ needs grid — set in SETUP row 2");
                s_last_focus_drawn = -1;
                return;
            }
            snprintf(wire, sizeof(wire), "CQ CQ CQ %.4s", cfg->grid);
            break;
        default:
            ESP_LOGE(TAG, "invoke_row: bad row %d", row);
            return;
    }

    ESP_LOGI(TAG, "Row %d (%s): sending '%s'",
             row, ROW_LABEL[row], wire);

    esp_err_t err = nanojs8_tx_audio_transmit_text(wire);
    if (err != ESP_OK) {
        // L7.13-fix3: at this point all four known precondition
        // failures (init/UTC/radio/callsign) have been pre-checked
        // above with specific banners. Reaching this branch means a
        // race won between our pre-check and transmit_text's own
        // check (e.g. operator opened SETUP and cleared callsign in
        // the few-ms window) — or a less-common failure like
        // tx_plan malloc OOM. Log the specific err code so it's
        // diagnosable from serial; show a generic on-screen banner.
        ESP_LOGE(TAG,
            "Row %d (%s): transmit_text failed unexpectedly: %s (0x%x). "
            "All preconditions passed local pre-check; race or OOM "
            "likely.",
            row, ROW_LABEL[row], esp_err_to_name(err), (int)err);
        set_error("TX failed — see serial log");
        s_last_focus_drawn = -1;
        return;
    }

    // L7.11f-fix2c: log to DIRECTED so operator sees their own
    // broadcast appear alongside incoming replies. Per-row fields:
    //   HEARTBEAT → @HB  HEARTBEAT  <grid|"">
    //   QUERY     → @ALLCALL  QUERY  MSGS
    //   CQ        → @ALLCALL  CQ     <grid4>
    // Mirrors MicroJS8 app.py _hb_log_outgoing / record_out paths.
    switch (row) {
        case 0:  // HEARTBEAT
            nanojs8_activity_record_out(
                "@HB", "HEARTBEAT",
                has_grid ? cfg->grid : "");
            break;
        case 1:  // QUERY MSGS
            nanojs8_activity_record_out("@ALLCALL", "QUERY", "MSGS");
            break;
        case 2: {  // CQ — wire is "CQ CQ CQ <grid4>"; log as @ALLCALL CQ grid4
            char grid4[5] = {0};
            if (has_grid) {
                strncpy(grid4, cfg->grid, 4);
                grid4[4] = '\0';
            }
            nanojs8_activity_record_out("@ALLCALL", "CQ", grid4);
            break;
        }
        default:
            // invoke_row already bailed for bad rows above; defensive
            // nothing-to-log here.
            break;
    }

    // L7.11f-fix2c: redirect to DIRECTED after every successful ALLCALL
    // transmit so the operator sees their own broadcast + any incoming
    // replies. Mirrors COMPOSE → DIRECTED behavior shipped in
    // L7.11f-fix1.
    ESP_LOGI(TAG, "Row %d (%s) queued — auto-switching to DIRECTED",
             row, ROW_LABEL[row]);
    nanojs8_ui_set_screen(NANOJS8_SCREEN_DIRECTED);
}

// ── Input handling ───────────────────────────────────────────────────

bool handle_input(uint8_t event) {
    // LEFT → COMPOSE (previous on ring), RIGHT → HOME (closes the ring).
    if (event == NANOJS8_TRACKBALL_LEFT) {
        s_right_ticks = 0;
        s_up_ticks = s_down_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            // L7.11f: COMPOSE was inserted between DIRECTED and
            // ALLCALL — LEFT from ALLCALL goes to COMPOSE, not DIRECTED.
            nanojs8_ui_set_screen(NANOJS8_SCREEN_COMPOSE);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        s_left_ticks = 0;
        s_up_ticks = s_down_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_HOME);
        }
        return true;
    }

    // UP/DOWN: cycle focus across the 3 rows (wraps).
    if (event == NANOJS8_TRACKBALL_UP) {
        s_down_ticks = 0;
        s_left_ticks = s_right_ticks = 0;
        if (++s_up_ticks >= FOCUS_TICK_THRESHOLD) {
            s_up_ticks = 0;
            s_focus_row = (s_focus_row - 1 + ROWS_COUNT) % ROWS_COUNT;
            ESP_LOGI(TAG, "Focus -> %d (%s)",
                     s_focus_row, ROW_LABEL[s_focus_row]);
        }
        return true;
    }
    if (event == NANOJS8_TRACKBALL_DOWN) {
        s_up_ticks = 0;
        s_left_ticks = s_right_ticks = 0;
        if (++s_down_ticks >= FOCUS_TICK_THRESHOLD) {
            s_down_ticks = 0;
            s_focus_row = (s_focus_row + 1) % ROWS_COUNT;
            ESP_LOGI(TAG, "Focus -> %d (%s)",
                     s_focus_row, ROW_LABEL[s_focus_row]);
        }
        return true;
    }

    // Enter or trackball CLICK invokes the focused row. Both are
    // supported because the keyboard's Enter is the canonical
    // confirm gesture (SETUP screen uses it) and the trackball
    // CLICK is the no-keyboard fallback (e.g. operator using the
    // device one-handed).
    if (event == '\r' || event == NANOJS8_TRACKBALL_CLICK) {
        invoke_row(s_focus_row);
        return true;
    }

    return false;
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_ALLCALL = {
    .id           = NANOJS8_SCREEN_ALLCALL,
    .name         = "ALLCALL",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
