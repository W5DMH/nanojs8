// NanoJS8 — Config schema
//
// Single source of truth for the persistent configuration shape. The
// schema starts small in Phase 1 (CALL/GRID/RADIO) and grows over
// time as later phases add fields. Each shape change is a version
// bump tracked here.
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
//
// Version history:
//   v1 (Phase 1) — callsign, grid, radio
//   v2 (Phase 2) — added groups field
//   v3 (Phase 3a) — added radio_autostart bool (auto-start radio service on boot)
//   v4 (Phase 3.5) — added power mgmt: idle_dim_sec, idle_off_sec, dim_brightness
#define NANOJS8_CONFIG_VERSION 4

// Field length limits, including the null terminator. These match the
// physical SETUP screen layout (240×135 with the chosen font) and the
// ITU amateur-callsign maximum length.
//
// Callsign:  up to 15 chars (e.g. "DL/W5DMH/MM" + slack). ITU caps at
//            10 real chars but slash-suffix (/P /M /MM /AM) and country-
//            prefix-prepend (DL/W5DMH/P) eat the rest.
// Grid:      Maidenhead 4 or 6 chars (e.g. "EM10" or "EM10aa").
// Radio:     short enum-name string (e.g. "qdx", "g90_digirig").
// Groups:    Comma-separated list of up to 4 group memberships, each
//            "@" + 1..9 uppercase alphanumerics. Worst case is
//            "@XXXXXXXXX,@XXXXXXXXX,@XXXXXXXXX,@XXXXXXXXX" = 43 chars.
//            Buffer is 64 for headroom and clean memcpy alignment.
//            Empty string ("") means no groups (legal — the operator
//            has no group memberships beyond the implicit @ALLCALL/@HB
//            which are handled at the protocol layer).
#define NANOJS8_CALLSIGN_MAXLEN  16   // 15 chars + NUL
#define NANOJS8_GRID_MAXLEN       8   // 7 chars + NUL
#define NANOJS8_RADIO_MAXLEN     20   // 19 chars + NUL
#define NANOJS8_GROUPS_MAXLEN    64   // 43 + NUL with margin

// Maximum number of operator-configured groups in the GROUPS string.
// Matches MicroJS8's MAX_GROUPS = 4. The protocol-level implicit groups
// @ALLCALL and @HB are NOT counted toward this limit — they're always
// active and never user-configured.
#define NANOJS8_MAX_GROUPS        4

// Per-group-entry length: "@" + 1..9 alphanumerics = 2..10 chars on the
// wire. Matches the MicroJS8 _GROUP_RE regex.
#define NANOJS8_GROUP_ENTRY_MAXLEN 10

// Valid radio profile string values. The SETUP screen's "Radio" menu
// cycles through these in order. Adding a new profile means appending
// to this list AND adding the profile handler in Phase 3.
//
// Why string-by-value rather than enum: NVS-stored strings are
// inspectable from any tool that mounts the partition, and could be
// printed verbatim in serial logs. The runtime cost of strcmp() at
// config-load is negligible (microseconds, once per boot).
extern const char* const NANOJS8_RADIO_PROFILES[];
extern const size_t      NANOJS8_RADIO_PROFILES_COUNT;

// Defaults applied on first boot (when NVS has no nanojs8 namespace
// yet) and as the value highlighted as "current selection" for menu
// fields.
//
// "NOCALL" is the FCC/ITU-recognized placeholder convention for
// amateur software that needs a non-empty callsign before the
// operator has entered theirs. Later phases (Phase 3+) check for
// this exact string and refuse to enable TX while it's set, with
// an on-screen banner.
#define NANOJS8_DEFAULT_CALLSIGN "NOCALL"
#define NANOJS8_DEFAULT_GRID     "AA00"
#define NANOJS8_DEFAULT_RADIO    "qdx"
#define NANOJS8_DEFAULT_GROUPS   ""

// v3+: radio autostart. When true, main() calls radio_service::start()
// after config load. When false (default), the operator must run the
// `radio start` serial command. The default is intentionally OFF so a
// fresh device boots into the same state Phase 0/1/2 firmware booted
// into — no surprise USB activity.
#define NANOJS8_DEFAULT_RADIO_AUTOSTART false

// v4+: power management (Phase 3.5).
//   idle_dim_sec   — seconds idle before the screen dims (0 = disabled)
//   idle_off_sec   — seconds idle before the screen blanks (0 = disabled)
//   dim_brightness — backlight percent (0-100) while dimmed
// Defaults: dim at 2 min, blank at 5 min, dim to 30%. Tuned for field
// battery saving without being annoying during active use. Blanking is
// display-only and never interrupts the radio service.
#define NANOJS8_DEFAULT_IDLE_DIM_SEC   120
#define NANOJS8_DEFAULT_IDLE_OFF_SEC   300
#define NANOJS8_DEFAULT_DIM_BRIGHTNESS 30

// The Config struct. Plain-old-data so it can be memcpy'd or zero-init'd
// without ceremony. All strings are NUL-terminated, fixed-size arrays
// rather than std::string — std::string's heap behavior is unwanted on
// a config struct that lives for the whole program lifetime.
struct Config {
    uint32_t version;                              // == NANOJS8_CONFIG_VERSION when valid
    char     callsign[NANOJS8_CALLSIGN_MAXLEN];
    char     grid    [NANOJS8_GRID_MAXLEN];
    char     radio   [NANOJS8_RADIO_MAXLEN];
    char     groups  [NANOJS8_GROUPS_MAXLEN];      // v2+: "@A,@B,@C" or empty
    bool     radio_autostart;                      // v3+: auto-start radio service on boot
    uint16_t idle_dim_sec;                         // v4+: idle seconds before dim (0=off)
    uint16_t idle_off_sec;                         // v4+: idle seconds before blank (0=off)
    uint8_t  dim_brightness;                        // v4+: backlight % when dimmed
};
