// NanoJS8 — PlaceholderScreen
//
// A single screen implementation used for HEARD, DIRECTED, INBOX,
// COMPOSE, and ALLCALL during Phase 2. Each instance is constructed
// with its ScreenId, header label, and a "Phase N pending" note
// shown in dim text below the header. The router instantiates one
// per screen.
//
// When a real implementation lands (Phases 4-6), the corresponding
// PlaceholderScreen instance is replaced with the concrete class in
// the router's screen registry. No interface changes are required
// because they all share IScreen.

#pragma once

#include "screen_router.h"

namespace nanojs8 {
namespace ui {
namespace screens {

class PlaceholderScreen : public IScreen {
public:
    // Construct a placeholder.
    //   id          : the ScreenId this instance represents
    //   header      : the all-caps screen name shown in green at top
    //   phase_note  : a short message describing when this lands
    //                 (e.g. "Phase 4 — audio decode pending")
    PlaceholderScreen(ScreenId id, const char* header, const char* phase_note);

    ScreenId id() const override { return id_; }
    void on_enter() override;
    bool handle_event(const InputEvent& ev) override;
    void render() override;

private:
    ScreenId    id_;
    const char* header_;
    const char* phase_note_;
    bool        needs_redraw_;
};

} // namespace screens
} // namespace ui
} // namespace nanojs8
