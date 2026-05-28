// NanoJS8 — HOME screen implementation.

#include "screens/home_screen.h"

#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include <M5Cardputer.h>

#include "config_store.h"
#include "radio_service.h"

namespace nanojs8 {
namespace ui {
namespace screens {

static const char* TAG = "home";

// Layout constants — derived from the 240×135 Cardputer ADV display.
// Status rows match SetupScreen's FIELD_ROW_H spacing for visual
// consistency.
namespace layout {
    constexpr int SCREEN_W      = 240;
    constexpr int SCREEN_H      = 135;
    constexpr int HEADER_X      = 12;
    constexpr int HEADER_Y      = 2;
    constexpr int ROWS_Y        = 22;
    constexpr int ROW_H         = 12;
    constexpr int LABEL_X       = 12;
    constexpr int VALUE_X       = 80;
    constexpr int EXIT_BUTTON_X = 95;
    constexpr int EXIT_BUTTON_Y = 110;
    constexpr int EXIT_BUTTON_W = 50;
    constexpr int EXIT_BUTTON_H = 14;
    constexpr int FOOTER_X      = 12;
    constexpr int FOOTER_Y      = 126;
}

HomeScreen::HomeScreen()
    : exit_focused_(false)
    , needs_redraw_(true)
    , last_snapshot_ms_(0)
    , ptt_active_cached_(false)
{
    std::strncpy(cat_text_,  "Disconnected", sizeof(cat_text_)  - 1);
    cat_text_ [sizeof(cat_text_)  - 1] = '\0';
    std::strncpy(freq_text_, "----",         sizeof(freq_text_) - 1);
    freq_text_[sizeof(freq_text_) - 1] = '\0';
}

void HomeScreen::on_enter() {
    exit_focused_ = false;
    needs_redraw_ = true;
    ESP_LOGI(TAG, "HOME screen entered");
}

bool HomeScreen::handle_event(const InputEvent& ev) {
    switch (ev.key) {
        case Key::TAB:
            // Toggle focus on/off the EXIT button. Two-state cycle
            // because EXIT is the only focusable widget here.
            exit_focused_ = !exit_focused_;
            needs_redraw_ = true;
            return true;

        case Key::ENTER:
            if (exit_focused_) {
                // Phase 2 stub. Phase 7 will route this to a
                // confirmation modal that either halts the device
                // safely or reboots to bootloader.
                ESP_LOGI(TAG, "EXIT pressed — deferred to Phase 7 (no-op for now)");
            }
            return true;

        case Key::LEFT:
        case Key::RIGHT:
            // Ring-nav events bubble up to the router unchanged.
            // Returning false lets the router handle them.
            return false;

        default:
            break;
    }
    // Anything else (typed chars, UP/DOWN, etc.): consume silently
    // so they don't trigger unwanted ring nav.
    return true;
}

void HomeScreen::render() {
    // Phase 3a: refresh the CAT / FREQ row caches from radio_service
    // snapshot at most every 500 ms. Doing this even when not redrawing
    // means we notice changes and force a redraw at the next tick.
    constexpr uint32_t SNAPSHOT_INTERVAL_MS = 500;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((now_ms - last_snapshot_ms_) >= SNAPSHOT_INTERVAL_MS) {
        last_snapshot_ms_ = now_ms;

        nanojs8::radio::Snapshot snap;
        nanojs8::radio::snapshot(&snap);

        // CAT row: use the status_text which already includes the PTT
        // indicator and profile name when connected, or the generic
        // disconnected/error/etc. message otherwise.
        char new_cat[sizeof(cat_text_)];
        std::strncpy(new_cat, snap.status_text, sizeof(new_cat) - 1);
        new_cat[sizeof(new_cat) - 1] = '\0';

        char new_freq[sizeof(freq_text_)];
        if (snap.supports_freq && snap.freq_hz != 0) {
            // Format as e.g. "14.078 MHz". Avoid floating point — the
            // ESP32-S3 has FPU but printf with %.3f drags a lot of code.
            const uint32_t mhz_int    = snap.freq_hz / 1000000;
            const uint32_t mhz_frac   = (snap.freq_hz % 1000000) / 1000;
            std::snprintf(new_freq, sizeof(new_freq),
                          "%lu.%03lu MHz",
                          (unsigned long)mhz_int, (unsigned long)mhz_frac);
        } else {
            std::strncpy(new_freq, "----", sizeof(new_freq) - 1);
            new_freq[sizeof(new_freq) - 1] = '\0';
        }

        if (std::strcmp(new_cat, cat_text_) != 0 ||
            std::strcmp(new_freq, freq_text_) != 0 ||
            snap.ptt_active != ptt_active_cached_) {
            std::strncpy(cat_text_,  new_cat,  sizeof(cat_text_)  - 1);
            cat_text_ [sizeof(cat_text_)  - 1] = '\0';
            std::strncpy(freq_text_, new_freq, sizeof(freq_text_) - 1);
            freq_text_[sizeof(freq_text_) - 1] = '\0';
            ptt_active_cached_ = snap.ptt_active;
            needs_redraw_ = true;
        }
    }

    if (!needs_redraw_) {
        return;
    }
    draw_full();
    needs_redraw_ = false;
}

void HomeScreen::draw_full() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);

    // Header.
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(layout::HEADER_X, layout::HEADER_Y);
    d.printf("HOME");

    // Status rows. Live values from NVS for fields we have; placeholder
    // strings for fields not wired in Phase 2.
    const Config& cfg = nanojs8::config::current();

    struct Row {
        const char* label;
        const char* value;
        uint16_t    value_color;
    };

    // Build status rows. Some values are conditional: callsign in
    // red if it's the NOCALL placeholder, otherwise cyan. CAT/FREQ
    // come from the radio_service snapshot polled in render() above.
    const bool no_callsign = nanojs8::config::is_default_callsign();
    const bool radio_ok = (std::strcmp(cat_text_, "Disconnected") != 0 &&
                            std::strcmp(cat_text_, "Error")        != 0);
    const uint16_t cat_color  = radio_ok
                                  ? (ptt_active_cached_ ? (uint16_t)TFT_RED
                                                        : (uint16_t)TFT_GREEN)
                                  : (uint16_t)TFT_DARKGREY;
    const uint16_t freq_color = radio_ok ? (uint16_t)TFT_CYAN
                                          : (uint16_t)TFT_DARKGREY;
    Row rows[] = {
        { "CALL ",  cfg.callsign,
            no_callsign ? (uint16_t)TFT_RED : (uint16_t)TFT_CYAN },
        { "GRID ",  cfg.grid,           TFT_CYAN     },
        { "GPS  ",  "No fix",           TFT_DARKGREY },
        { "FREQ ",  freq_text_,         freq_color   },
        { "CAT  ",  cat_text_,          cat_color    },
        { "INBOX", "0 unread",          TFT_DARKGREY },
    };
    constexpr size_t row_count = sizeof(rows) / sizeof(rows[0]);

    d.setTextSize(1);
    for (size_t i = 0; i < row_count; ++i) {
        const int y = layout::ROWS_Y + (int)i * layout::ROW_H;
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(layout::LABEL_X, y);
        d.printf("%s:", rows[i].label);

        d.setTextColor(rows[i].value_color, TFT_BLACK);
        d.setCursor(layout::VALUE_X, y);
        d.printf("%s", rows[i].value);
    }

    // EXIT button — outlined rectangle with centered text.
    const uint16_t btn_bg     = exit_focused_ ? TFT_YELLOW : TFT_BLACK;
    const uint16_t btn_fg     = exit_focused_ ? TFT_BLACK  : TFT_WHITE;
    const uint16_t btn_border = exit_focused_ ? TFT_YELLOW : TFT_WHITE;
    d.fillRect(layout::EXIT_BUTTON_X, layout::EXIT_BUTTON_Y,
               layout::EXIT_BUTTON_W, layout::EXIT_BUTTON_H, btn_bg);
    d.drawRect(layout::EXIT_BUTTON_X, layout::EXIT_BUTTON_Y,
               layout::EXIT_BUTTON_W, layout::EXIT_BUTTON_H, btn_border);
    d.setTextColor(btn_fg, btn_bg);
    d.setCursor(layout::EXIT_BUTTON_X + 12, layout::EXIT_BUTTON_Y + 4);
    d.printf("EXIT");

    // Footer — ring nav hint.
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(layout::FOOTER_X, layout::FOOTER_Y);
    d.printf("< prev   > next");
}

} // namespace screens
} // namespace ui
} // namespace nanojs8
