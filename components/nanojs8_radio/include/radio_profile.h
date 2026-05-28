// NanoJS8 — Radio profile catalog
//
// Mirrors MicroJS8's microjs8.cat.radios.RadioDef dataclass shape, in
// C++. Each entry describes ONE supported radio configuration:
//   - identity / display name
//   - USB enumeration hints (VID:PID where deterministic)
//   - PTT mechanism (CAT command vs RTS line toggle)
//   - timing parameters (settle delays, TX pipeline latency)
//   - audio binding (mono/stereo, sample rate)
//
// Adding a new profile is a code change: extend the catalog in
// radio_profile.cpp, increment the count, and the SETUP screen's
// RADIO menu picks it up automatically.
//
// Phase 3a ships ONE profile: digirig_unknown. The catalog deliberately
// lists qdx, g90_digirig, and (tr)usdx as "future profiles" with their
// metadata filled in but flagged unsupported_in_phase = 3a — those
// entries exist so the SETUP screen cycle order doesn't have to change
// when later phases enable them.

#pragma once

#include <cstdint>
#include <cstddef>

namespace nanojs8 {
namespace radio {

// ---------------------------------------------------------------------------
// PTT method
// ---------------------------------------------------------------------------
//
// CAT: PTT is asserted via a CAT command on the radio's serial port.
//      For QMX/QDX this is "TX;" / "RX;" Kenwood TS-480 commands.
//      Phase 3c work.
//
// RTS: PTT is asserted by raising the RS-232 RTS control line of the
//      USB-UART bridge. The radio's interface hardware (DigiRig, KH1,
//      generic optoisolator) routes RTS to its PTT input.
//      Phase 3a uses this exclusively.
//
// NONE: Profile has no PTT capability (placeholder for future receive-
//       only profiles).
enum class PttMethod : uint8_t {
    NONE = 0,
    RTS  = 1,
    CAT  = 2,
};

// ---------------------------------------------------------------------------
// USB enumeration match
// ---------------------------------------------------------------------------
//
// Each profile has 1..N "USB device matches" — VID:PID combinations the
// host should accept as "this is the expected device" for the profile.
// PID==0 means "any product id from this vendor matches" (used when a
// vendor uses many PIDs for the same chip family — e.g. CP210x ships
// with many factory-programmed PIDs).
struct UsbDeviceMatch {
    uint16_t    vid;
    uint16_t    pid;           // 0 = wildcard
    const char* description;   // for diagnostic logging only
};

// ---------------------------------------------------------------------------
// Audio format hint
// ---------------------------------------------------------------------------
//
// What sample rate / bit depth / channel count to request from the
// device's UAC interface. The ESP32-S3 USB FIFO budget is the binding
// constraint here:
//
//   mono 16-bit  48 kHz  ->  96 bytes/dir/ms   (DigiRig CM108)   ✓ fits
//   mono 24-bit  48 kHz  -> 144 bytes/dir/ms                     ✓ fits
//   stereo 16-bit 48 kHz -> 192 bytes/dir/ms                     ✓ fits
//   stereo 24-bit 48 kHz -> 288 bytes/dir/ms   (QMX, QDX)        ✗ blows budget
//
// In Phase 3a we only need the first row.
struct AudioFormat {
    uint32_t sample_rate_hz;
    uint8_t  bit_depth;
    uint8_t  channels;
};

// ---------------------------------------------------------------------------
// Radio profile descriptor
// ---------------------------------------------------------------------------
//
// One entry per supported radio configuration. POD-style; no virtuals,
// no dynamic allocation. The whole catalog lives in flash as a const
// array.
struct RadioProfile {
    // Identity & display
    const char* id;            // matches NVS "radio" field; e.g. "digirig_unknown"
    const char* display_name;  // shown on HOME CAT row; e.g. "DigiRig RTS-PTT"
    const char* description;   // long-form, used in serial-monitor `radio status`

    // Phase gating. If !is_supported_now, the SETUP screen's RADIO menu
    // still lists it but the radio service refuses to start with it
    // selected and falls back to Disconnected with a helpful log.
    bool is_supported_now;

    // CAT capability
    bool cat_required;         // some radios need CAT to function (QDX/QMX)
    bool cat_provides_freq;    // true if profile supports live frequency read

    // PTT mechanism
    PttMethod ptt_method;
    uint16_t  ptt_on_delay_ms;
    uint16_t  ptt_off_delay_ms;

    // PTT safety: max time PTT may stay asserted before auto-release.
    // MicroJS8 hard-codes 20 s; we match that to keep behavior identical
    // when this Cardputer accidentally hangs in a TX state.
    uint16_t  ptt_max_hold_s;

    // USB serial-bridge VID:PID match list. Order = preference (first
    // match wins).
    const UsbDeviceMatch* serial_matches;
    size_t                serial_match_count;

    // Audio format we ask the UAC device to deliver.
    AudioFormat audio;

    // CDC line coding for the serial port (only meaningful when the
    // serial port is opened — DigiRig RTS-only profiles still need
    // a sane line coding because some CP2102 firmware versions stall
    // if line_coding_set() is omitted).
    uint32_t baud_rate;
};

// ---------------------------------------------------------------------------
// Catalog access
// ---------------------------------------------------------------------------
//
// Returns the profile matching the given id (typically the string read
// from NVS), or nullptr if no profile has that id.
const RadioProfile* lookup_by_id(const char* id);

// Returns the catalog and its size. Useful for diagnostic dumps.
const RadioProfile* catalog(size_t* out_count);

} // namespace radio
} // namespace nanojs8
