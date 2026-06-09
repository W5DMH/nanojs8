/*
 * radio.h — NanoJS8 v0.7 radio profile registry (Layer 6b.5)
 * ============================================================
 * A radio profile describes everything NanoJS8 needs to know about a
 * specific radio in order to talk to it: how PTT is keyed (RTS line,
 * DTR line, CAT command, or audio-VOX), which CAT protocol it speaks
 * (if any), the default CAT baud rate, CI-V address bytes (for Icom-
 * style radios), and capability flags.
 *
 * The registry is a small, compile-time array — no NVS, no dynamic
 * loading. Adding a new radio means adding a struct entry and
 * rebuilding. This keeps the path short, the resource use predictable,
 * and the failure modes obvious.
 *
 * Layering
 * ────────
 *   nanojs8_radio       ── stores profiles, lookups by id (this file)
 *   nanojs8_ptt         ── reads the active profile, drives serial
 *                          RTS/DTR with a 20s watchdog and the
 *                          profile's settle delays
 *   nanojs8_cat         ── facade: picks CI-V backend based on the
 *                          profile's cat enum
 *   nanojs8_cat_civ     ── Icom CI-V frame builder/parser
 *   nanojs8_usb_serial  ── actually toggles the hardware lines and
 *                          carries the CI-V bytes
 *
 * The radio component itself never touches hardware. It is pure data.
 *
 * L6b.5 profile set (two entries):
 *   - digirig-rts-only    (RTS PTT only, no CAT)   — default
 *   - xiegu-g90-digirig   (RTS PTT + Icom CI-V CAT at 19200, 0x70)
 *
 * The trusdx-ts480 and ts480 profiles from L6b.4 were removed in
 * L6b.5: the (tr)uSDX has no CAT path through DigiRig (its TRRS jack
 * routes only audio + PTT, not CAT serial), and direct USB-CAT on
 * the (tr)uSDX firmware 2.00x is known-unreliable. The Kenwood TS-480
 * radio itself remains a valid future target, but its ASCII protocol
 * is a different code path from CI-V and is deferred until we have a
 * unit to test against.
 *
 * Profile selection
 * ─────────────────
 * The user chooses a profile in the SETUP screen's Radio row via the
 * picker (L6b.4-hotfix2). Trackball UP/DOWN cycles, CLICK commits.
 * Default at boot: digirig-rts-only.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// How PTT is keyed for this radio.
typedef enum {
    // No PTT support — audio is the only output. Almost no real radios
    // use this; reserved for future "monitor only" profiles.
    NANOJS8_RADIO_PTT_NONE = 0,

    // RTS line on the CP2102 asserts during transmit. This is the
    // DigiRig wiring for nearly every modern rig.
    NANOJS8_RADIO_PTT_RTS,

    // DTR line on the CP2102 asserts during transmit. Some older
    // CAT cables route PTT this way.
    NANOJS8_RADIO_PTT_DTR,

    // PTT keyed via a CAT command. Reserved for future profiles where
    // no hardware PTT line exists.
    NANOJS8_RADIO_PTT_CAT,
} nanojs8_radio_ptt_t;

// CAT protocol family.
typedef enum {
    // No CAT — radio is "dumb" from our perspective (e.g. FM walkies
    // wired through DigiRig with audio + PTT only).
    NANOJS8_RADIO_CAT_NONE = 0,

    // Icom CI-V — hex-framed protocol used by Icom and Icom-compatible
    // rigs including the Xiegu G90, X6100, X5105, IC-7100, IC-7300,
    // IC-705, etc. Frames are FE FE <to> <from> <cmd> [data] FD with
    // BCD-encoded frequencies.
    NANOJS8_RADIO_CAT_CIV,
} nanojs8_radio_cat_t;

// One radio's profile. POD struct so the registry can be a static array.
typedef struct {
    // ── Identity / display ─────────────────────────────────────────
    const char *id;            // matches NVS radio_id (snake-case)
    const char *display_name;  // for UI (short, ≤20 chars for HOME row)
    const char *description;   // longer human-readable description

    // ── PTT control ────────────────────────────────────────────────
    nanojs8_radio_ptt_t ptt;
    // Settle delays around PTT transitions. Read by nanojs8_ptt to
    // bracket the actual transmit window. 0 = no delay applied.
    //
    // - ptt_on_delay_ms : time to wait AFTER asserting PTT before
    //   audio/CAT may begin. Lets the radio's relay close and the
    //   internal mode-switch complete before we modulate.
    // - ptt_off_delay_ms : time to wait AFTER finishing audio before
    //   releasing PTT. Lets the audio tail drain through the modulator
    //   so we don't truncate the trailing samples.
    //
    // These map directly to MicroJS8's ptt_on_delay_ms / off_delay_ms
    // fields. Values come from MicroJS8's per-radio empirical tuning.
    uint16_t ptt_on_delay_ms;
    uint16_t ptt_off_delay_ms;

    // ── CAT control ────────────────────────────────────────────────
    nanojs8_radio_cat_t cat;
    uint32_t  cat_baud;          // baud for CAT (0 if cat==NONE)
    bool      can_set_freq;      // CAT supports frequency change?

    // CI-V address bytes. Only meaningful when cat == CAT_CIV.
    //   cat_civ_radio_addr : the radio's CI-V address (G90 default 0x70)
    //   cat_civ_ctrl_addr  : our controller address (typically 0xE0,
    //                        the "generic controller" address that
    //                        every CI-V radio knows to ignore in echoes)
    uint8_t   cat_civ_radio_addr;
    uint8_t   cat_civ_ctrl_addr;
} nanojs8_radio_profile_t;

// ---------------------------------------------------------------------------
// Registry API
// ---------------------------------------------------------------------------

// Number of registered profiles. Currently 2 (L6b.5).
size_t nanojs8_radio_count(void);

// Return the profile at index [0, count). Returns NULL if out of range.
// Order is the order they appear in radio.cpp (default profile first).
const nanojs8_radio_profile_t *nanojs8_radio_at(size_t index);

// Find a profile by id string (case sensitive — ids are canonical
// snake-case). Returns NULL if no match. O(N) scan over a tiny array.
const nanojs8_radio_profile_t *nanojs8_radio_lookup(const char *id);

// Find the registry-index of a profile by id. Returns the [0, count)
// index on success, or -1 if no profile has that id. Used by the
// SETUP picker (L6b.4-hotfix2) which cycles through the registry.
int nanojs8_radio_index_of(const char *id);

// Return the profile referenced by the current nanojs8_config_t.radio_id.
// If the stored id doesn't match any registered profile (e.g. a stale
// NVS entry from a removed profile such as 'ts480' or 'trusdx-ts480'),
// returns the default profile so the system always has SOMETHING to
// point at. The fallback is logged once.
const nanojs8_radio_profile_t *nanojs8_radio_get_active(void);

// Get the default profile. Stable pointer for the lifetime of the
// program (registry is static). Used as the fallback in
// nanojs8_radio_get_active() above.
const nanojs8_radio_profile_t *nanojs8_radio_get_default(void);

#ifdef __cplusplus
}
#endif
