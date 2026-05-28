// NanoJS8 — Radio service
//
// The orchestrator that ties together USB host, UAC audio, and CDC-ACM
// PTT into a single facade. The rest of the system (HOME screen for
// status, Phase 4 decoder for RX audio, Phase 5 modulator for TX +
// PTT) talks only to this API.
//
// Phase 3a state machine (per-profile, currently only digirig_unknown):
//
//                          start()
//                            ▼
//                   ┌─────────────────┐
//      stop()    ──>│   IDLE          │  No USB activity. Default at boot.
//                   └────────┬────────┘
//                            │  start() → switch USB PHY → install host stack
//                            ▼
//                   ┌─────────────────┐
//                   │  ENUMERATING    │  Waiting for device on the bus.
//                   └────────┬────────┘
//                            │  device appears, matches profile VID:PID
//                            ▼
//                   ┌─────────────────┐
//                   │   CONNECTED     │  UAC streaming, CDC open, PTT
//                   │                 │  ready. HOME shows live state.
//                   └────────┬────────┘
//                            │  device removed
//                            ▼
//                   ┌─────────────────┐
//                   │   ENUMERATING   │  (loop back; await next plug)
//                   └─────────────────┘
//
//                ERROR can be entered from any state when the host stack
//                rejects an enumeration or a critical transfer aborts.
//                ERROR auto-clears back to ENUMERATING after 5 seconds
//                (the underlying USB devices may have power glitches we
//                want to recover from automatically).
//
// PHY switching:
//
//   The first call to start() flips the ESP32-S3's internal USB PHY
//   from USB-Serial-JTAG to USB-OTG. This is a one-way operation per
//   boot — calling stop() does NOT switch back, because the host PC's
//   monitor session has already been torn down. To restore the monitor,
//   power-cycle the device. This matches Mini-FT8's "HOST mode" workflow.
//
//   The PHY switch is loud: a warning is logged before the switch and
//   given 50 ms to flush over USB-Serial-JTAG so the operator sees it.
//
// Threading:
//
//   radio_service runs a USB-host task and one task per active stream
//   (UAC RX, UAC TX), all pinned to core 1. The UI task on core 0 polls
//   snapshot() at 2 Hz for HOME rendering — this is a lock-free read of
//   atomics, so it never blocks the UI.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace nanojs8 {
namespace radio {

// Coarse-grained state visible to the rest of the system.
enum class Status : uint8_t {
    IDLE         = 0,   // Service not started (default at boot)
    ENUMERATING  = 1,   // Service running, waiting for device
    CONNECTED    = 2,   // Device enumerated, streams open, ready to use
    ERROR        = 3,   // Recoverable error; will retry in 5 s
};

// Lifecycle.
//
// start() is idempotent: calling it when already started is a logged
// no-op. The PHY switch only happens on the FIRST start() per boot.
//
// stop() releases USB resources and transitions to IDLE. The PHY is
// NOT switched back — monitor is gone until power cycle.
//
// Returns:
//   ESP_OK              — service is running (or was already running)
//   ESP_ERR_INVALID_STATE — selected radio profile is not yet supported
//                          (e.g. NVS has radio="qdx" in Phase 3a, where
//                          only digirig_unknown is supported)
//   ESP_ERR_NO_MEM      — USB host install failed for OOM reasons
//   ESP_FAIL            — generic underlying USB host install failure
esp_err_t start(void);
esp_err_t stop(void);

// Current state (lock-free atomic read; safe from any task).
Status status(void);

// Diagnostic snapshot for HOME rendering and serial-monitor commands.
// All fields are populated atomically (race-free).
struct Snapshot {
    Status   status;
    char     profile_id    [24];   // "digirig_unknown"
    char     display_name  [32];   // "DigiRig RTS-PTT"
    char     status_text   [40];   // "Disconnected", "DigiRig RTS-PTT", etc.
    bool     ptt_active;
    bool     supports_freq;        // false for digirig_unknown
    uint32_t freq_hz;              // 0 when !supports_freq

    // Diagnostic counters.
    uint32_t rx_frames_total;
    uint32_t rx_overruns;
    uint32_t enum_attempts;        // number of times a device appeared
    uint32_t last_event_ms;        // millis() of last enum/disconn
};

void snapshot(Snapshot* out);

// PTT control.
//
// Returns ESP_OK on success, ESP_ERR_INVALID_STATE if not CONNECTED.
//
// ptt_on() asserts the radio profile's configured PTT mechanism. For
// digirig_unknown that's the CP2102 RTS line. After ptt_on_delay_ms,
// the radio is keyed and ready to receive audio (Phase 5).
//
// ptt_off() releases. After ptt_off_delay_ms, the radio is back in RX.
//
// PTT safety: if PTT remains asserted longer than the profile's
// ptt_max_hold_s (default 20 s), the service auto-releases and logs
// at WARN level. This matches MicroJS8's safety watchdog.
esp_err_t ptt_on(void);
esp_err_t ptt_off(void);

// RX audio access. Reads up to max_samples 16-bit mono samples at
// the profile's configured rate (48 kHz for digirig_unknown).
// Returns the actual count delivered. Non-blocking; returns 0 if no
// audio buffered.
//
// Phase 4 decoder will call this from its own task. Phase 3a only
// uses it for diagnostic purposes via serial-monitor commands.
size_t rx_read(int16_t* dest, size_t max_samples);

// Drop all buffered RX audio. Used internally on disconnect and by
// diagnostic commands. Does NOT reset the frames_total counter.
void rx_drain(void);

// TX audio sink. Phase 5 modulator writes here; in Phase 3a this is
// a no-op stub that returns sample_count (pretending to consume) but
// writes nothing to the radio. Once the UAC TX stream is wired in
// Phase 5, the bytes will reach the radio.
size_t tx_write(const int16_t* src, size_t sample_count);

} // namespace radio
} // namespace nanojs8
