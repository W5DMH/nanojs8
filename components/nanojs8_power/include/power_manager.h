// NanoJS8 — Power management subsystem
//
// Owns all battery and power-state concerns for the device. The rest of
// the system queries this module rather than touching M5Cardputer.Power
// directly, so the smoothing, thresholds, and mode logic live in one
// place.
//
// Why this exists (Phase 3.5):
//   NanoJS8 is a dedicated-firmware appliance, not an app launched from
//   M5's launcher. That means NanoJS8 owns power/charge management —
//   there's no launcher underneath to handle it. The Cardputer ADV also
//   has hardware quirks that make this non-trivial:
//     - Charge IC pulls only ~300 mA from USB regardless of supply.
//     - Running firmware consumes most of that, so the battery charges
//       very slowly (or not at all) while the screen is on.
//     - Single USB-C port can't host a radio AND charge simultaneously.
//     - Hardware can't read charge current or charging state — only
//       battery level (%) and voltage (mV).
//
// Features:
//   1. Smoothed battery telemetry (rolling average over raw samples)
//   2. Charge mode: screen fully off to maximize charge current to the
//      battery (~8 hr full charge vs ~28 hr with screen on)
//   3. Idle auto-dim/blank: after configurable timeouts with no user
//      activity, dim then blank the screen to save battery
//   4. Low/critical battery thresholds with load-shedding (stop radio)
//
// Threading: a monitor task samples the battery every few seconds and
// drives the idle timer. The UI task queries snapshot()/level() lock-free
// for the HOME indicator. Keypresses call notify_activity() to reset the
// idle timer and wake the screen.

#pragma once

#include <cstdint>

namespace nanojs8 {
namespace power {

// Battery level classification. Thresholds are in battery_pct().
enum class Level : uint8_t {
    NORMAL   = 0,   // > LOW threshold
    LOW      = 1,   // <= 20%
    CRITICAL = 2,   // <= 10%
};

// Screen power state driven by the idle timer / charge mode.
enum class ScreenState : uint8_t {
    FULL     = 0,   // Normal brightness
    DIMMED   = 1,   // Idle-dimmed (partial brightness)
    BLANKED  = 2,   // Idle-blanked (backlight off, panel may still be awake)
    CHARGE   = 3,   // Charge mode: panel asleep, backlight off, UI suspended
};

struct Snapshot {
    int         battery_pct;     // 0-100, smoothed
    int         battery_mv;      // smoothed millivolts
    Level       level;
    ScreenState screen_state;
    bool        in_charge_mode;
    uint32_t    idle_sec;        // seconds since last activity
};

// Runtime-adjustable settings (loaded from NVS, tweakable via serial).
struct Settings {
    uint16_t idle_dim_sec;       // seconds idle before dimming (0 = disabled)
    uint16_t idle_off_sec;       // seconds idle before blanking (0 = disabled)
    uint8_t  dim_brightness;     // 0-100, brightness when dimmed
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialize and start the power monitor task. Must be called after
// M5Cardputer.begin() (needs the power/display drivers up) and after
// config load (reads settings from NVS). Idempotent.
void init(void);

// ---------------------------------------------------------------------------
// Telemetry (lock-free reads, safe from any task)
// ---------------------------------------------------------------------------

int   battery_pct(void);      // 0-100, smoothed
int   battery_mv(void);       // smoothed millivolts
Level level(void);
void  snapshot(Snapshot* out);

// ---------------------------------------------------------------------------
// Charge mode
// ---------------------------------------------------------------------------

// Enter charge mode: stops the radio service (sheds load), puts the
// display panel to sleep, suspends UI rendering. CPU stays at full
// clock so the serial console stays readable. Maximizes charge current
// to the battery.
//
// Entered via Ctrl+C on the Cardputer keyboard (production) or the
// `charge` serial command (development).
void enter_charge_mode(void);

// Exit charge mode: wakes the panel, restores brightness, resumes UI.
// Triggered by any keypress or the `charge off` serial command.
void exit_charge_mode(void);

bool in_charge_mode(void);

// ---------------------------------------------------------------------------
// Activity / idle management
// ---------------------------------------------------------------------------

// Called on any user input (keypress) to reset the idle timer and wake
// the screen from dim/blank. Also exits charge mode if active. Safe to
// call from the input path on every keypress.
void notify_activity(void);

// Query whether UI rendering should be suppressed right now (true when
// blanked or in charge mode). The UI task checks this before drawing to
// avoid wasting cycles rendering to a dark screen.
bool ui_rendering_suppressed(void);

// Apply any pending screen-state transition (brightness / panel sleep).
// MUST be called only from the UI task — it is the single owner of the
// display bus. M5GFX is not thread-safe, so all setBrightness/sleep/
// wakeup calls funnel through here. The power monitor task and charge/
// activity functions only record the DESIRED state; this applies it.
// Call once per UI tick, before rendering.
void apply_pending_screen_change(void);

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

const Settings& settings(void);
void set_idle_dim_sec(uint16_t sec);    // persists to NVS
void set_idle_off_sec(uint16_t sec);    // persists to NVS
void set_dim_brightness(uint8_t pct);   // persists to NVS

} // namespace power
} // namespace nanojs8
