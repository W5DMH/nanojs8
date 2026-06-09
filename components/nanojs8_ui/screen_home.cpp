/*
 * screen_home.cpp — NanoJS8 v0.7 HOME screen (L7.12: MicroJS8-style redesign)
 * =================================================================
 * The default screen once the station is configured. Redesigned in
 * L7.12 to drop diagnostic rows (last keypress, trackball counters)
 * and adopt the left-aligned "Label  Value" key-value layout from
 * MicroJS8's home screen.
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ NanoJS8 v0.7                       12:34:56Z HOME    │
 *   │ ────────────────────────────────────────────────     │
 *   │   Stn      W5DMH at EN83IH                           │
 *   │   Freq     7.078 MHz                                 │
 *   │   Radio    DigiRig RTS                               │
 *   │   CAT      CONNECTED  (7.078 MHz)                    │
 *   │   GPS      (pending — L7.8)                          │
 *   │   PTT      idle  tx:5 wdt:0                          │
 *   │   Audio    48 kHz stereo  RX:Y TX:Y                  │
 *   │   Serial   9600 8N1                                  │
 *   │   Mailbox  3 unread / 12 total                       │
 *   └──────────────────────────────────────────────────────┘
 *
 * Input behavior (unchanged from L7.11g.3):
 *   - TRACKBALL_RIGHT → switch to SETUP screen (4-tick debounce)
 *   - TRACKBALL_LEFT  → switch to DIRECTED screen (4-tick debounce)
 *   - T / t  → PTT test pulse (1 s)
 *   - P / p  → PTT toggle
 *   - X / x  → slot-aligned on-air JS8 HEARTBEAT (TXes for real)
 *   - F / f  → (removed L6b.6, CAT now auto-probes)
 *
 * License: GPL-3.0
 */

#include "ui.h"
#include "ui_internal.h"

#include "display.h"
#include "trackball.h"
#include "config.h"
#include "audio.h"
#include "usb_serial.h"
#include "keyboard.h"
#include "radio.h"
#include "ptt.h"
#include "gps.h"
#include "cat.h"
#include "time_source.h"
#include "tx_audio.h"
#include "mailbox.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "screen_home";

namespace {

// ── Geometry ─────────────────────────────────────────────────────────
constexpr int SCREEN_W = NANOJS8_DISPLAY_WIDTH;   // 320
constexpr int SCREEN_H = NANOJS8_DISPLAY_HEIGHT;  // 240
constexpr int FONT_H   = 16;
constexpr int FONT_W   = 8;
constexpr int PAD_X    = 6;

// Header band — version on the left, UTC clock + "HOME" mode tag on
// the right. Identical to the L6b/L7.0 layout that's already proven
// reliable; only the body below the separator changes in L7.12.
constexpr int HEADER_Y     = 6;
constexpr int SEPARATOR_Y  = HEADER_Y + FONT_H + 4;   // 26

// Body layout — 9 KV rows at 20 px stride starts 4 px below separator.
//   9 rows × 20 px = 180 px
//   header (30 px) + body (180 px) = 210 px → 30 px footroom on 240 px
constexpr int ROW_STRIDE   = FONT_H + 4;              // 20
constexpr int BODY_Y0      = SEPARATOR_Y + 4;         // 30

// Per-row Y coordinates — computed at compile time so any future
// re-ordering is one-place.
constexpr int row_y(int idx) { return BODY_Y0 + idx * ROW_STRIDE; }
constexpr int ROW_STN_Y      = row_y(0);
constexpr int ROW_FREQ_Y     = row_y(1);
constexpr int ROW_RADIO_Y    = row_y(2);
constexpr int ROW_CAT_Y      = row_y(3);
constexpr int ROW_GPS_Y      = row_y(4);
constexpr int ROW_PTT_Y      = row_y(5);
constexpr int ROW_AUDIO_Y    = row_y(6);
constexpr int ROW_SERIAL_Y   = row_y(7);
constexpr int ROW_MAILBOX_Y  = row_y(8);

// Label/value split. The label column occupies 8 character cells
// (64 px), matching MicroJS8's label_w=64. The longest label we use
// is "Mailbox" (7 chars) which leaves one cell of trailing space.
constexpr int LABEL_X        = PAD_X;
constexpr int LABEL_W_CHARS  = 8;
constexpr int VALUE_X        = LABEL_X + LABEL_W_CHARS * FONT_W;     // 70
constexpr int VALUE_W_CHARS  = (SCREEN_W - VALUE_X - PAD_X) / FONT_W; // 30
static_assert(LABEL_W_CHARS > 0 && VALUE_W_CHARS > 0,
              "Row geometry must leave room for both label and value");

// ── Colors ───────────────────────────────────────────────────────────
constexpr uint16_t COL_BG          = NANOJS8_COLOR_BLACK;
constexpr uint16_t COL_HEADER_NAME = NANOJS8_COLOR_YELLOW;   // "NanoJS8 v0.7"
constexpr uint16_t COL_HEADER_TAG  = NANOJS8_COLOR_CYAN;     // "HOME" + UTC
constexpr uint16_t COL_SEPARATOR   = NANOJS8_COLOR_DARK_GRAY;
constexpr uint16_t COL_LABEL       = NANOJS8_COLOR_GRAY;     // dimmed labels

// Status palette — semantic, reused across rows.
constexpr uint16_t COL_OK          = NANOJS8_COLOR_GREEN;    // configured / connected
constexpr uint16_t COL_INFO        = NANOJS8_COLOR_CYAN;     // informational / fresh data
constexpr uint16_t COL_WARN        = NANOJS8_COLOR_YELLOW;   // waiting / near-watchdog / unread mail
constexpr uint16_t COL_BAD         = NANOJS8_COLOR_RED;      // misconfigured / no reply / keyed PTT
constexpr uint16_t COL_DIM         = NANOJS8_COLOR_DARK_GRAY;// off / empty / not yet present

// ── Switch-debounce state (L7.10) ────────────────────────────────────
constexpr int SWITCH_TICK_THRESHOLD = 4;
int s_left_ticks  = 0;
int s_right_ticks = 0;

// ── Drawing primitives ───────────────────────────────────────────────
//
// draw_padded — write `text` to (x,y) as exactly `width_chars`
// character cells, padding right with spaces. Single SPI transaction:
// each glyph cell paints its own COL_BG background as part of the
// glyph render, so the trailing spaces overwrite any leftover pixels
// from a previous (longer) value without a separate clear pass.
//
// This is the same anti-flicker pattern proven in the L6b
// draw_row_centered helper, just specialized for a sub-region of a
// row instead of the whole row.
void draw_padded(int x, int y, const char* text, int width_chars,
                 uint16_t fg) {
    if (width_chars <= 0) return;
    char buf[64];
    if (width_chars >= (int)sizeof(buf)) {
        width_chars = (int)sizeof(buf) - 1;
    }
    int n = (int)strlen(text);
    if (n > width_chars) n = width_chars;
    memcpy(buf, text, n);
    for (int i = n; i < width_chars; ++i) buf[i] = ' ';
    buf[width_chars] = '\0';
    nanojs8_display_draw_text(x, y, buf, fg, COL_BG);
}

// draw_kv_row — Label-Value row primitive.
//
// Two SPI transactions per row (one for the label segment, one for
// the value segment). Each transaction writes a padded char-cell
// region whose trailing spaces clear any leftover pixels from a
// previous render. The two segments do NOT overlap (label spans
// cells 0..7, value spans cells 8..37 with PAD_X margin), so neither
// segment ever "blanks" a region that the other will write — there
// is no visible flicker between the two writes.
void draw_kv_row(int y, const char* label, const char* value,
                 uint16_t value_color) {
    draw_padded(LABEL_X, y, label,  LABEL_W_CHARS, COL_LABEL);
    draw_padded(VALUE_X, y, value,  VALUE_W_CHARS, value_color);
}

// ── Row renderers ────────────────────────────────────────────────────

// Stn — combined call + grid, like MicroJS8. Highlights any
// missing-config state in red so the operator sees it immediately.
void render_stn_row() {
    const nanojs8_config_t* cfg = nanojs8_config_get();
    const bool call_ok = (cfg->callsign[0] != '\0' &&
                          strcmp(cfg->callsign, "N0CALL") != 0);
    const bool grid_ok = (cfg->grid[0] != '\0');

    char value[40];
    uint16_t color;
    if (call_ok && grid_ok) {
        snprintf(value, sizeof(value), "%.10s at %.6s",
                 cfg->callsign, cfg->grid);
        color = COL_OK;
    } else if (!call_ok && !grid_ok) {
        snprintf(value, sizeof(value), "(unset) at (unset)");
        color = COL_BAD;
    } else if (!call_ok) {
        snprintf(value, sizeof(value), "(unset) at %.6s", cfg->grid);
        color = COL_BAD;
    } else {
        snprintf(value, sizeof(value), "%.10s at (unset)", cfg->callsign);
        color = COL_BAD;
    }
    draw_kv_row(ROW_STN_Y, "Stn", value, color);
}

// Freq — configured operating frequency. The CAT row separately
// shows the radio-reported freq; this row is "what we INTEND to be on".
void render_freq_row() {
    const nanojs8_config_t* cfg = nanojs8_config_get();
    char value[32];
    if (cfg->freq_hz == 0) {
        snprintf(value, sizeof(value), "(unset)");
        draw_kv_row(ROW_FREQ_Y, "Freq", value, COL_BAD);
        return;
    }
    // Format MHz.kHz with 3 fractional digits, no float arithmetic.
    unsigned mhz = (unsigned)(cfg->freq_hz / 1000000ULL);
    unsigned khz = (unsigned)((cfg->freq_hz / 1000ULL) % 1000ULL);
    snprintf(value, sizeof(value), "%u.%03u MHz", mhz, khz);
    draw_kv_row(ROW_FREQ_Y, "Freq", value, COL_OK);
}

// Radio — active profile's human display name. Red when no profile
// is active (shouldn't happen after L6b but guards the path).
void render_radio_profile_row() {
    const nanojs8_radio_profile_t* p = nanojs8_radio_get_active();
    if (!p) {
        draw_kv_row(ROW_RADIO_Y, "Radio", "(none)", COL_BAD);
        return;
    }
    char value[32];
    snprintf(value, sizeof(value), "%.28s",
             p->display_name ? p->display_name : "(none)");
    draw_kv_row(ROW_RADIO_Y, "Radio", value, COL_OK);
}

// CAT — link status to the radio. Reflects the CAT facade's state:
//   "CONNECTED  (7.078 MHz)"   fresh response, freq known
//   "waiting..."               request in flight
//   "no reply"                 timed out
//   "off"                      profile has no CAT (digirig-rts-only etc.)
//   "→ Connect DigiRig"        CAT profile active but serial not READY
void render_cat_row() {
    nanojs8_cat_status_t status = nanojs8_cat_status();
    const nanojs8_radio_profile_t* p = nanojs8_radio_get_active();
    const bool cat_profile_active =
        (p && p->cat == NANOJS8_RADIO_CAT_CIV);

    // L6b.6 special-case: CAT profile is active, but USB serial isn't
    // ready yet — surface a direct prompt rather than a confusing
    // "no reply".
    if (cat_profile_active && status != NANOJS8_CAT_STATUS_OK) {
        nanojs8_serial_info_t ser = {};
        nanojs8_serial_get_info(&ser);
        if (ser.status != NANOJS8_SERIAL_STATUS_READY) {
            draw_kv_row(ROW_CAT_Y, "CAT", "Connect DigiRig", COL_WARN);
            return;
        }
    }

    char value[40];
    uint16_t color;
    switch (status) {
    case NANOJS8_CAT_STATUS_OK: {
        uint64_t hz = nanojs8_cat_last_freq_hz();
        unsigned mhz = (unsigned)(hz / 1000000ULL);
        unsigned khz = (unsigned)((hz / 1000ULL) % 1000ULL);
        snprintf(value, sizeof(value),
                 "CONNECTED  %u.%03u MHz", mhz, khz);
        color = COL_INFO;
        break;
    }
    case NANOJS8_CAT_STATUS_WAITING:
        snprintf(value, sizeof(value), "waiting...");
        color = COL_WARN;
        break;
    case NANOJS8_CAT_STATUS_NO_REPLY:
        snprintf(value, sizeof(value), "no reply");
        color = COL_BAD;
        break;
    case NANOJS8_CAT_STATUS_OFF:
    default:
        snprintf(value, sizeof(value), "off");
        color = COL_DIM;
        break;
    }
    draw_kv_row(ROW_CAT_Y, "CAT", value, color);
}

// GPS — L7.14-fix2: current UTC + status annotation.
//
// The HH:MM:SS portion is the same time source as the top-of-HOME
// clock — nanojs8_time_get_utc() — which is the GPS-anchored,
// esp_timer-driven UTC the radio actually uses for JS8 slot
// alignment. It ticks every second whether or not we currently
// have GPS lock; the annotation tells the operator how cold the
// GPS link is:
//
//   DISABLED                          : "(off)"                dim
//   NO_FIX (no UTC yet)               : "no module"            dim
//   NO_FIX (UTC was manually set)     : "HH:MM:SS no module"   dim
//   SEARCHING (UTC not yet set)       : "searching"            yellow
//   SEARCHING (UTC set, prior lock)   : "HH:MM:SS hold Ns"     yellow
//   LOCKED                            : "HH:MM:SS fix Ns"      green
//
// L7.14-fix2 changes: previously the HH:MM:SS in this row was the
// snapshot of the LAST FIX TIME (frozen), so when the M10Q lost
// satellite lock indoors and stopped sending valid RMCs, this row
// showed a stale time + a growing age while the top-of-HOME clock
// was still ticking correctly. Confusing. Now both displays show
// the same maintained UTC.
void render_gps_row() {
    nanojs8_gps_snapshot_t snap;
    nanojs8_gps_get_snapshot(&snap);

    // Current UTC — same source as top-of-HOME clock.
    uint8_t cur_h = 0, cur_m = 0, cur_s = 0;
    const bool have_utc = nanojs8_time_get_utc(&cur_h, &cur_m, &cur_s);

    char        buf[32];
    const char *value;
    uint16_t    color;

    // Age display: clamp UINT32_MAX → 0 so we never print a giant
    // sentinel into the UI. last_fix_age_ms == UINT32_MAX means
    // "no valid fix has ever been seen this session" — we won't
    // be rendering an age string in that branch anyway.
    const uint32_t age_s = (snap.last_fix_age_ms == UINT32_MAX)
                           ? 0U
                           : snap.last_fix_age_ms / 1000U;

    switch (snap.status) {
        case NANOJS8_GPS_DISABLED:
            value = "(off)";
            color = COL_DIM;
            break;
        case NANOJS8_GPS_NO_FIX:
            if (have_utc) {
                snprintf(buf, sizeof(buf), "%02u:%02u:%02u no module",
                         cur_h, cur_m, cur_s);
                value = buf;
            } else {
                value = "no module";
            }
            color = COL_DIM;
            break;
        case NANOJS8_GPS_SEARCHING:
            if (have_utc) {
                // We've had a fix (UTC anchored) but no fresh RMC
                // in the last 30 s. Show "hold Ns" annotation.
                snprintf(buf, sizeof(buf), "%02u:%02u:%02u hold %us",
                         cur_h, cur_m, cur_s, (unsigned)age_s);
                value = buf;
            } else {
                value = "searching";
            }
            color = COL_WARN;
            break;
        case NANOJS8_GPS_LOCKED:
            if (have_utc) {
                snprintf(buf, sizeof(buf), "%02u:%02u:%02u fix %us",
                         cur_h, cur_m, cur_s, (unsigned)age_s);
                value = buf;
            } else {
                // Shouldn't normally happen — LOCKED means we
                // just anchored UTC from a valid RMC — but if the
                // time-component getter races, fall back gracefully.
                value = "locked";
            }
            color = COL_OK;
            break;
        default:
            value = "(unknown)";
            color = COL_DIM;
            break;
    }

    draw_kv_row(ROW_GPS_Y, "GPS", value, color);
}

// PTT — idle / KEYED / near-watchdog. Same color escalation as
// the previous render_ptt_row: gray → red → yellow as the watchdog
// approaches.
void render_ptt_row() {
    bool     keyed     = nanojs8_ptt_is_keyed();
    uint64_t held_ms   = nanojs8_ptt_keyed_ms();
    uint32_t total_tx  = nanojs8_ptt_total_tx();
    uint32_t wdt_trips = nanojs8_ptt_watchdog_trips();

    char value[40];
    uint16_t color;
    if (keyed) {
        const bool near_wdt =
            (held_ms + 5000) >= (uint64_t)NANOJS8_PTT_WATCHDOG_MS;
        color = near_wdt ? COL_WARN : COL_BAD;
        snprintf(value, sizeof(value),
                 "KEYED %u.%us  tx:%u wdt:%u",
                 (unsigned)(held_ms / 1000),
                 (unsigned)((held_ms / 100) % 10),
                 (unsigned)total_tx,
                 (unsigned)wdt_trips);
    } else {
        color = COL_DIM;
        snprintf(value, sizeof(value),
                 "idle  tx:%u wdt:%u",
                 (unsigned)total_tx,
                 (unsigned)wdt_trips);
    }
    draw_kv_row(ROW_PTT_Y, "PTT", value, color);
}

// Audio — USB audio device status. Simplified from the L6b layout:
// drop the peak meter (it lives in the heartbeat log) and keep just
// the operationally-important "what rate/channel are we running at,
// and are RX and TX both alive". Dim when no device is connected.
void render_audio_row() {
    nanojs8_audio_stream_info_t rx = {}, tx = {};
    nanojs8_audio_rx_info(&rx);
    nanojs8_audio_tx_info(&tx);

    const bool rx_ready = (rx.status == NANOJS8_AUDIO_STATUS_READY);
    const bool tx_ready = (tx.status == NANOJS8_AUDIO_STATUS_READY);

    if (!rx_ready && !tx_ready) {
        draw_kv_row(ROW_AUDIO_Y, "Audio", "waiting for USB...", COL_DIM);
        return;
    }
    uint32_t rate = rx_ready ? rx.sample_rate : tx.sample_rate;
    uint8_t  ch   = rx_ready ? rx.channels    : tx.channels;
    const char* ch_str = (ch == 1) ? "mono"   :
                         (ch == 2) ? "stereo" : "?";
    char value[40];
    snprintf(value, sizeof(value),
             "%lu Hz %s  RX:%c TX:%c",
             (unsigned long)rate, ch_str,
             rx_ready ? 'Y' : '-',
             tx_ready ? 'Y' : '-');
    draw_kv_row(ROW_AUDIO_Y, "Audio", value, COL_OK);
}

// Serial — USB CDC/CP2102 status. Simplified: just rate and frame
// when ready; byte counters live in the heartbeat log.
void render_serial_row() {
    nanojs8_serial_info_t ser = {};
    nanojs8_serial_get_info(&ser);

    if (ser.status != NANOJS8_SERIAL_STATUS_READY) {
        draw_kv_row(ROW_SERIAL_Y, "Serial", "waiting for CP2102", COL_DIM);
        return;
    }
    char parity_ch =
        (ser.parity == NANOJS8_SERIAL_PARITY_NONE)  ? 'N' :
        (ser.parity == NANOJS8_SERIAL_PARITY_ODD)   ? 'O' :
        (ser.parity == NANOJS8_SERIAL_PARITY_EVEN)  ? 'E' :
        (ser.parity == NANOJS8_SERIAL_PARITY_MARK)  ? 'M' :
        (ser.parity == NANOJS8_SERIAL_PARITY_SPACE) ? 'S' : '?';
    int stop_d = (ser.stop_bits == NANOJS8_SERIAL_STOP_2) ? 2 : 1;
    char value[32];
    snprintf(value, sizeof(value),
             "%lu %u%c%d  CP2102",
             (unsigned long)ser.baud_rate,
             ser.data_bits, parity_ch, stop_d);
    draw_kv_row(ROW_SERIAL_Y, "Serial", value, COL_INFO);
}

// Mailbox — same three states as the prior render, in KV format:
//   "3 unread / 12 total"    (yellow — attention)
//   "12 messages, all read"  (cyan — informational, no action)
//   "empty"                  (dim)
void render_mailbox_row() {
    const uint32_t total  = nanojs8_mailbox_count();
    const uint32_t unread = nanojs8_mailbox_count_unread();

    char value[48];
    uint16_t color;
    if (total == 0) {
        snprintf(value, sizeof(value), "empty");
        color = COL_DIM;
    } else if (unread == 0) {
        snprintf(value, sizeof(value),
                 "%u messages, all read", (unsigned)total);
        color = COL_INFO;
    } else {
        snprintf(value, sizeof(value),
                 "%u unread / %u total",
                 (unsigned)unread, (unsigned)total);
        color = COL_WARN;
    }
    draw_kv_row(ROW_MAILBOX_Y, "Mailbox", value, color);
}

// ── Header chrome ────────────────────────────────────────────────────
//
// Paint the static header band: "NanoJS8 v0.7" left (yellow),
// "HOME" right (cyan), separator line. The UTC clock between them
// is painted dynamically by render_utc_clock() once per frame.
void paint_chrome() {
    nanojs8_display_clear(COL_BG);

    const char* title = "NanoJS8 v0.7";
    nanojs8_display_draw_text(PAD_X, HEADER_Y, title,
                              COL_HEADER_NAME, COL_BG);

    const char* mode = "HOME";
    int mode_w = nanojs8_display_text_width(mode);
    nanojs8_display_draw_text(SCREEN_W - PAD_X - mode_w, HEADER_Y,
                              mode, COL_HEADER_TAG, COL_BG);

    nanojs8_display_fill_rect(0, SEPARATOR_Y, SCREEN_W, 1, COL_SEPARATOR);
}

// UTC clock — anchored just left of the "HOME" mode tag, repainted
// every render so the seconds tick. Empty when UTC isn't yet set so
// we don't show a misleading "00:00:00Z".
constexpr int UTC_GAP_PX = 12;
void render_utc_clock() {
    uint8_t h = 0, m = 0, s = 0;
    bool is_set = nanojs8_time_get_utc(&h, &m, &s);

    const char* mode = "HOME";
    int mode_w  = nanojs8_display_text_width(mode);
    int clock_w = nanojs8_display_text_width("00:00:00Z");
    int clock_x = SCREEN_W - PAD_X - mode_w - UTC_GAP_PX - clock_w;
    int clock_y = HEADER_Y;

    nanojs8_display_fill_rect(clock_x, clock_y, clock_w, FONT_H, COL_BG);
    if (!is_set) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02uZ",
             (unsigned)h, (unsigned)m, (unsigned)s);
    nanojs8_display_draw_text(clock_x, clock_y, buf,
                              COL_HEADER_TAG, COL_BG);
}

// ── Screen lifecycle ─────────────────────────────────────────────────

void on_enter() {
    ESP_LOGI(TAG, "Entering HOME");
    // L7.10: reset switch-debounce so a flick that just landed us on
    // HOME doesn't carry over a partial counter.
    s_left_ticks  = 0;
    s_right_ticks = 0;
    paint_chrome();
}

void render(bool full_redraw) {
    (void)full_redraw;  // on_enter() already painted chrome; rows
                        // self-clear via padded-glyph background.
    render_utc_clock();
    render_stn_row();
    render_freq_row();
    render_radio_profile_row();
    render_cat_row();
    render_gps_row();
    render_ptt_row();
    render_audio_row();
    render_serial_row();
    render_mailbox_row();
}

// ── Input handling — unchanged from L7.11e ───────────────────────────

bool handle_input(uint8_t event) {
    // HOME → SETUP via TRACKBALL_RIGHT (debounced, L7.10)
    if (event == NANOJS8_TRACKBALL_RIGHT) {
        s_left_ticks = 0;
        if (++s_right_ticks >= SWITCH_TICK_THRESHOLD) {
            s_right_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_SETUP);
        }
        return true;
    }
    // HOME → DIRECTED via TRACKBALL_LEFT
    if (event == NANOJS8_TRACKBALL_LEFT) {
        s_right_ticks = 0;
        if (++s_left_ticks >= SWITCH_TICK_THRESHOLD) {
            s_left_ticks = 0;
            nanojs8_ui_set_screen(NANOJS8_SCREEN_DIRECTED);
        }
        return true;
    }

    // PTT verification hotkeys (L6b.4) — refuse mid-TX (L7.11d).
    if (event == 'T' || event == 't') {
        if (nanojs8_tx_audio_is_active()) {
            ESP_LOGW(TAG, "Hotkey '%c': TX in progress, refusing PTT pulse",
                     event);
            return true;
        }
        ESP_LOGI(TAG, "Hotkey '%c': PTT test pulse (1s)", event);
        if (nanojs8_ptt_set(true) == ESP_OK) {
            xTaskCreate([](void*){
                vTaskDelay(pdMS_TO_TICKS(1000));
                nanojs8_ptt_set(false);
                vTaskDelete(nullptr);
            }, "ptt_pulse", 2048, nullptr, 3, nullptr);
        }
        return true;
    }
    if (event == 'P' || event == 'p') {
        if (nanojs8_tx_audio_is_active()) {
            ESP_LOGW(TAG, "Hotkey '%c': TX in progress, refusing PTT toggle",
                     event);
            return true;
        }
        bool was = nanojs8_ptt_is_keyed();
        ESP_LOGI(TAG, "Hotkey '%c': PTT toggle (%s -> %s)",
                 event, was ? "KEYED" : "idle", was ? "idle" : "KEYED");
        nanojs8_ptt_set(!was);
        return true;
    }

    // L7.11e: slot-aligned on-air JS8 HEARTBEAT shortcut. Same path
    // as ALLCALL → HEARTBEAT → Enter; kept here for bench muscle memory.
    if (event == 'X' || event == 'x') {
        if (nanojs8_tx_audio_is_active()) {
            ESP_LOGW(TAG, "Hotkey '%c': TX already in flight, ignoring",
                     event);
        } else {
            const nanojs8_config_t *cfg = nanojs8_config_get();
            char wire[40];
            if (cfg && cfg->grid[0] != '\0') {
                snprintf(wire, sizeof(wire),
                         "@HB HEARTBEAT %.4s", cfg->grid);
            } else {
                snprintf(wire, sizeof(wire), "@HB HEARTBEAT");
            }
            ESP_LOGI(TAG, "Hotkey '%c': slot-aligned TX '%s' "
                          "(PTT will key)", event, wire);
            esp_err_t err = nanojs8_tx_audio_transmit_text(wire);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Hotkey '%c': transmit_text failed "
                              "(0x%x) — check UTC + radio profile + "
                              "callsign", event, (int)err);
            }
        }
        return true;
    }
    return false;  // not consumed
}

} // anonymous namespace

extern const nanojs8_screen_t SCREEN_HOME = {
    .id           = NANOJS8_SCREEN_HOME,
    .name         = "HOME",
    .render       = render,
    .handle_input = handle_input,
    .on_enter     = on_enter,
};
