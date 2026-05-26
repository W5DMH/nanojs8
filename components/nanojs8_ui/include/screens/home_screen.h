// NanoJS8 — HOME screen.
//
// First screen of the ring; the at-a-glance operator status view.
// Shown after boot when the callsign is configured (not "NOCALL").
//
// Layout (240×135 landscape):
//
//   +----------------------------------------+
//   | HOME                                   |   header
//   |                                        |
//   |   CALL :  W5DMH                        |   from NVS
//   |   GRID :  EM10                         |   from NVS
//   |   GPS  :  No fix                       |   Phase 6 fills in
//   |   FREQ :  ----                         |   Phase 3 fills in
//   |   CAT  :  Disconnected                 |   Phase 3 fills in
//   |   INBOX:  0 unread                     |   Phase 6 fills in
//   |                                        |
//   |               [ EXIT ]                 |   focusable, Phase 7 wires
//   |                                        |
//   | < prev   > next                        |   ring nav hint
//   +----------------------------------------+
//
// In Phase 2, the only interactive widget is the EXIT button (Tab
// gives it focus; Enter on it is a logged no-op deferred to Phase 7).
// Status lines are static reads from NVS where data is available;
// placeholder strings ("----", "No fix", etc.) where it's not.

#pragma once

#include "screen_router.h"

namespace nanojs8 {
namespace ui {
namespace screens {

class HomeScreen : public IScreen {
public:
    HomeScreen();

    ScreenId id() const override { return ScreenId::HOME; }
    void on_enter() override;
    bool handle_event(const InputEvent& ev) override;
    void render() override;
    // HOME never enters text-edit mode in Phase 2, so the default
    // is_editing_text() == false is correct.

private:
    // Phase 2 only has one focusable element (EXIT). When Tab is
    // pressed, focus toggles between "no focus" (status rows are
    // implicitly highlighted via NVS values) and "EXIT focused".
    bool exit_focused_;
    bool needs_redraw_;

    void draw_full();
};

} // namespace screens
} // namespace ui
} // namespace nanojs8
