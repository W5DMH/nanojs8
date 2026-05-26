// NanoJS8 — Config schema
//
// Single source of truth for the persistent configuration shape. The whole
// schema is intentionally tiny in Phase 1: only the fields the SETUP screen
// can edit live here. Later phases add fields (TX power, audio gain, GPS
// settings, etc.) and bump NANOJS8_CONFIG_VERSION.
//
// On version bump:
//   1. Increment NANOJS8_CONFIG_VERSION below.
//   2. Add new fields to the Config struct with sensible defaults.
//   3. Update config_store.cpp's load() to migrate from the previous
//      version (read old fields, default new ones, write back at new
//      version).
//
// Why a version field rather than NVS-namespace-per-version: keeps the
// migration path linear and avoids leaving stale namespaces behind.

#pragma once

#include <cstdint>
#include <cstddef>

// Schema version. Increment on any change to the Config struct layout
// or validation semantics. config_store::load() consults this on every
// boot to decide whether migration is needed.
#define NANOJS8_CONFIG_VERSION 1

// Field length limits, including the null terminator. These match the
// physical SETUP screen layout (240×135 with the chosen font) and the
// ITU amateur-callsign maximum length.
//
// Callsign:  up to 12 chars (e.g. "WG2XYZ/AM" + slack). ITU caps at 10
//            real chars but slash-suffix prefixes (/P, /M, /MM, /AM) and
//            country-prefix-prepend (DL/W5DMH/P) eat the rest.
// Grid:      Maidenhead 4 or 6 chars (e.g. "EM10" or "EM10aa").
// Radio:     short enum-name string (e.g. "qdx", "g90_digirig",
//            "digirig_unknown"). Stored as string for human-readable
//            NVS dumps via the DOCTOR screen.
#define NANOJS8_CALLSIGN_MAXLEN  16   // 15 chars + NUL
#define NANOJS8_GRID_MAXLEN       8   // 7 chars + NUL (4 or 6 used)
#define NANOJS8_RADIO_MAXLEN     20   // 19 chars + NUL

// Valid radio profile string values. The SETUP screen's "Radio" menu
// cycles through these in order. Adding a new profile means appending
// to this list AND adding the profile handler in Phase 3.
//
// Why string-by-value rather than enum: NVS-stored strings are
// inspectable from any tool that mounts the partition, and the DOCTOR
// screen can print them without a lookup table. The runtime cost of
// strcmp() at config-load is negligible (microseconds, once per boot).
extern const char* const NANOJS8_RADIO_PROFILES[];
extern const size_t      NANOJS8_RADIO_PROFILES_COUNT;

// Defaults applied on first boot (when NVS has no nanojs8 namespace yet)
// and as the value highlighted as "current selection" for menu fields.
//
// "NOCALL" is the FCC/ITU-recognized placeholder convention for amateur
// software that needs a non-empty callsign before the operator has
// entered theirs. Later phases (Phase 3+) check for this exact string
// and refuse to enable TX while it's set, with an on-screen banner.
#define NANOJS8_DEFAULT_CALLSIGN "NOCALL"
#define NANOJS8_DEFAULT_GRID     "AA00"
#define NANOJS8_DEFAULT_RADIO    "qdx"

// The Config struct. Plain-old-data so it can be memcpy'd or zero-init'd
// without ceremony. All strings are NUL-terminated, fixed-size arrays
// rather than std::string — std::string's heap behavior is unwanted on
// a config struct that lives for the whole program lifetime.
struct Config {
    uint32_t version;                              // == NANOJS8_CONFIG_VERSION when valid
    char     callsign[NANOJS8_CALLSIGN_MAXLEN];
    char     grid    [NANOJS8_GRID_MAXLEN];
    char     radio   [NANOJS8_RADIO_MAXLEN];
};
