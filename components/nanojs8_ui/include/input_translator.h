// NanoJS8 — Input translator.
//
// Polls M5Cardputer.Keyboard.keysState() each frame and produces zero or
// more InputEvent values for the screen router to dispatch.
//
// The translator is the only place that knows about M5Cardputer's
// keyboard API quirks (Fn-combo decoding, edge detection, debouncing).
// Everything downstream sees logical events only.
//
// Edge detection: keysState() reports the current pressed-state of every
// key in the matrix every poll. To avoid emitting "Tab" 60 times per
// second while the user holds Tab, we track the previous frame's state
// and emit events on press-edges only (no auto-repeat in Phase 1; can
// add per-key repeat later if Inbox scrolling needs it).

#pragma once

#include <cstddef>
#include "input_event.h"

namespace nanojs8 {
namespace ui {

// Poll the keyboard once and append any newly-pressed-edge events to
// `out`. Returns the number of events appended (typically 0 or 1; can
// be >1 if the user typed faster than the poll rate).
//
// Caller invokes this once per UI tick (~20 Hz). The first call after
// boot has no prior state, so it ignores any keys that happen to be
// held at power-on.
//
// Capacity: `out` must point to space for at least `out_cap` events.
// Excess events from the same poll are dropped (with a debug log) —
// this is fine because the matrix is small and the UI is single-step.
size_t poll(InputEvent* out, size_t out_cap);

// Reset translator state. Used after a screen transition to avoid
// edge-detection leak (e.g. Enter still showing as pressed when the
// router has already consumed it).
void reset();

} // namespace ui
} // namespace nanojs8
