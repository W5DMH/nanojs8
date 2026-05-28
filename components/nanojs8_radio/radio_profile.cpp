// NanoJS8 — Radio profile catalog implementation.
//
// One entry per supported (and planned-but-disabled) radio. Order MUST
// match config_store.cpp's NANOJS8_RADIO_PROFILES[] string list so the
// SETUP screen's RADIO menu and this catalog stay in sync.
//
// Profile IDs are stable strings; do NOT change them without writing
// a config-schema migration (NVS-stored values would otherwise stop
// matching).

#include "radio_profile.h"

#include <cstring>

namespace nanojs8 {
namespace radio {

// ---------------------------------------------------------------------------
// USB device match tables (per-profile)
// ---------------------------------------------------------------------------

// DigiRig CM108: standard PID for the audio side. The audio class
// component matches by USB Audio Class (0x01), not by VID:PID, so this
// list is informational only — actual UAC enumeration is class-based.
// But we keep it here so `radio status` can show "expected device" in
// diagnostic logs.

// CP2102/CP2102N (Silicon Labs USB-UART bridge used in DigiRig).
// The CP210x VCP driver matches by vendor 0x10C4 and a list of known
// PIDs — passing the umbrella does the right thing. The match list
// below is for our internal logging.
static const UsbDeviceMatch s_digirig_unknown_matches[] = {
    { 0x10C4, 0xEA60, "DigiRig CP2102 serial bridge (PTT/RTS)" },
    { 0x10C4, 0xEA70, "DigiRig CP2102N variant" },  // some newer DigiRigs
};

// Placeholder match table for future profiles. Identifies the radio
// even though we won't open the device in Phase 3a.
static const UsbDeviceMatch s_qdx_matches[] = {
    { 0x1209, 0x6052, "QRP Labs QDX (TS-480 emulation)" },
};
static const UsbDeviceMatch s_g90_digirig_matches[] = {
    { 0x10C4, 0xEA60, "DigiRig CP2102 (G90 control via DigiRig)" },
};

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------
//
// Phase 3a: only digirig_unknown is is_supported_now=true. The other
// entries exist so the SETUP screen RADIO menu cycle order is stable
// across phases — user-visible behavior is "you can select this profile
// but the radio service will refuse to start it until Phase 3b/3c."
//
// Order matches NANOJS8_RADIO_PROFILES[] in config_store.cpp.
static const RadioProfile s_catalog[] = {
    // ─────────────────────────────────────────────────────────────────────
    // qdx — Phase 3c (needs QMX/QDX-style TA CAT command path)
    {
        .id                = "qdx",
        .display_name      = "QRP Labs QDX",
        .description       = "QDX over UAC+CDC. RX via USB audio, TX via TA CAT. "
                             "Phase 3c.",
        .is_supported_now  = false,
        .cat_required      = true,
        .cat_provides_freq = true,
        .ptt_method        = PttMethod::CAT,
        .ptt_on_delay_ms   = 50,
        .ptt_off_delay_ms  = 100,
        .ptt_max_hold_s    = 20,
        .serial_matches    = s_qdx_matches,
        .serial_match_count= sizeof(s_qdx_matches)/sizeof(s_qdx_matches[0]),
        .audio             = { 48000, 16, 1 },  // mono RX (TX is via CAT)
        .baud_rate         = 9600,
    },
    // ─────────────────────────────────────────────────────────────────────
    // g90_digirig — Phase 3b/c (Xiegu G90 CAT + DigiRig RTS-PTT)
    {
        .id                = "g90_digirig",
        .display_name      = "Xiegu G90 + DigiRig",
        .description       = "G90 control via DigiRig CP2102 at 19200 baud, "
                             "PTT via RTS. Audio via DigiRig CM108. Phase 3b.",
        .is_supported_now  = false,
        .cat_required      = true,
        .cat_provides_freq = true,
        .ptt_method        = PttMethod::RTS,
        .ptt_on_delay_ms   = 300,
        .ptt_off_delay_ms  = 200,
        .ptt_max_hold_s    = 20,
        .serial_matches    = s_g90_digirig_matches,
        .serial_match_count= sizeof(s_g90_digirig_matches)/sizeof(s_g90_digirig_matches[0]),
        .audio             = { 48000, 16, 1 },
        .baud_rate         = 19200,
    },
    // ─────────────────────────────────────────────────────────────────────
    // digirig_unknown — Phase 3a (THE Phase 3a profile)
    {
        .id                = "digirig_unknown",
        .display_name      = "DigiRig RTS-PTT",
        .description       = "DigiRig Mobile with arbitrary radio. RTS-PTT "
                             "only (no CAT). Audio via DigiRig CM108. "
                             "Frequency / mode managed on radio's front panel.",
        .is_supported_now  = true,
        .cat_required      = false,
        .cat_provides_freq = false,
        .ptt_method        = PttMethod::RTS,
        // 300/200 ms matches MicroJS8 digirig-rts-only profile, which
        // is conservative for the slowest-PTT radios (FM walkies and
        // similar). Operators with fast radios won't notice the budget
        // impact unless they're chasing sub-1 second slot timing —
        // which Phase 3a doesn't do.
        .ptt_on_delay_ms   = 300,
        .ptt_off_delay_ms  = 200,
        .ptt_max_hold_s    = 20,
        .serial_matches    = s_digirig_unknown_matches,
        .serial_match_count= sizeof(s_digirig_unknown_matches)/sizeof(s_digirig_unknown_matches[0]),
        // DigiRig CM108: mono 16-bit at 48 kHz. wMaxPacketSize ≈ 96 B
        // per direction, which fits in the ESP32-S3 USB FIFO budget
        // (Balanced bias gives IN 408 / OUT 192) with massive headroom.
        .audio             = { 48000, 16, 1 },
        // CP2102 default baud. The exact value doesn't matter for
        // PTT-only operation (RTS is a separate control line, not a
        // data byte) but line_coding_set() requires SOMETHING.
        .baud_rate         = 9600,
    },
};

static const size_t s_catalog_count = sizeof(s_catalog) / sizeof(s_catalog[0]);

const RadioProfile* lookup_by_id(const char* id) {
    if (!id) {
        return nullptr;
    }
    for (size_t i = 0; i < s_catalog_count; ++i) {
        if (std::strcmp(s_catalog[i].id, id) == 0) {
            return &s_catalog[i];
        }
    }
    return nullptr;
}

const RadioProfile* catalog(size_t* out_count) {
    if (out_count) {
        *out_count = s_catalog_count;
    }
    return s_catalog;
}

} // namespace radio
} // namespace nanojs8
