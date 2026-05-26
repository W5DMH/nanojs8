// NanoJS8 — PlaceholderScreen implementation.

#include "screens/placeholder_screen.h"

#include <cstring>

#include "esp_log.h"
#include <M5Cardputer.h>

namespace nanojs8 {
namespace ui {
namespace screens {

static const char* TAG = "placeholder";

// Layout constants — match SetupScreen's visual style for ring
// consistency. The header is in green at size 2; the phase note is
// in dim grey at size 1 underneath. Footer shows the ring-nav hint.
namespace layout {
    constexpr int SCREEN_W   = 240;
    constexpr int SCREEN_H   = 135;
    constexpr int HEADER_X   = 12;
    constexpr int HEADER_Y   = 4;
    constexpr int NOTE_X     = 12;
    constexpr int NOTE_Y     = 36;
    constexpr int FOOTER_X   = 12;
    constexpr int FOOTER_Y   = 116;
}

PlaceholderScreen::PlaceholderScreen(ScreenId id,
                                     const char* header,
                                     const char* phase_note)
    : id_(id)
    , header_(header)
    , phase_note_(phase_note)
    , needs_redraw_(true)
{
}

void PlaceholderScreen::on_enter() {
    needs_redraw_ = true;
    ESP_LOGI(TAG, "%s screen entered", header_);
}

bool PlaceholderScreen::handle_event(const InputEvent& ev) {
    // Placeholders consume nothing — every event falls through to the
    // router for global handling (LEFT/RIGHT for ring nav, etc.).
    //
    // We could log unconsumed Enter/Tab presses for debugging but the
    // router already logs key names at DEBUG, so we stay quiet here.
    (void)ev;
    return false;
}

void PlaceholderScreen::render() {
    if (!needs_redraw_) {
        return;
    }
    auto& d = M5Cardputer.Display;

    d.fillScreen(TFT_BLACK);

    // Header — same style as SETUP for visual consistency.
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(layout::HEADER_X, layout::HEADER_Y);
    d.printf("%s", header_);

    // Phase-pending note.
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(layout::NOTE_X, layout::NOTE_Y);
    d.printf("%s", phase_note_);

    // Footer — ring-nav hint.
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(layout::FOOTER_X, layout::FOOTER_Y);
    d.printf("< prev   > next");

    needs_redraw_ = false;
}

} // namespace screens
} // namespace ui
} // namespace nanojs8
