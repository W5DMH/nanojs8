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
//   |   FREQ :  ----                         |   Phase 3c (CAT-providing profiles)
//   |   CAT  :  DigiRig RTS-PTT *TX*         |   Phase 3a: live from radio_service
//   |   INBOX:  0 unread                     |   Phase 6 fills in
//   |                                        |
//   |               [ EXIT ]                 |   focusable, Phase 7 wires
//   |                                        |
//   | < prev   > next                        |   ring nav hint
//   +----------------------------------------+
//
// Phase 3a wires CAT and FREQ to the radio_service snapshot, polled at
// 2 Hz from render(). FREQ stays "----" for profiles that don't read
// frequency from the radio (digirig_unknown). CAT shows the active
// profile's display name when connected, or status text otherwise.
// PTT-on appends " *TX*" in red.
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
    // Phase 2 only had one focusable element (EXIT). When Tab is
    // pressed, focus toggles between "no focus" (status rows are
    // implicitly highlighted via NVS values) and "EXIT focused".
    bool     exit_focused_;
    bool     needs_redraw_;

    // Phase 3a: cached radio snapshot for CAT / FREQ rows. Re-fetched
    // by render() at most every SNAPSHOT_INTERVAL_MS to avoid pounding
    // the snapshot path on every render tick.
    uint32_t last_snapshot_ms_;
    char     cat_text_[40];
    char     freq_text_[24];
    bool     ptt_active_cached_;

    void draw_full();
};

} // namespace screens
} // namespace ui
} // namespace nanojs8
