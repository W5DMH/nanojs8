/*
 * setup_validators.h — NanoJS8 v0.7 SETUP edit-mode validators (L6b.3)
 * =======================================================================
 * Per-field validation and parsing helpers used by screen_setup.cpp's
 * edit mode. Kept in a separate translation unit so screen_setup.cpp
 * doesn't balloon past ~500 lines as L6b.4/6b.5 add more screens.
 *
 * Validation philosophy (mirrors MicroJS8):
 *   - Validate strictly enough to prevent confusing on-air behavior
 *     (e.g. a non-callsign in the callsign field would break JS8
 *     macros), but permissively enough to accept the formats real
 *     operators type.
 *   - Always validate UPPER-case-normalized for callsign and grid
 *     prefix so the user can type either case at the keyboard.
 *
 * The validator functions take a NUL-terminated C string and return
 * true if the value is acceptable for that field. They do NOT mutate
 * the input — the caller is responsible for normalization on commit.
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

// Callsign: 3-10 chars, alphanumeric + optional '/' for portable
// (e.g. "W5DMH/M"). Must contain at least one digit. Case insensitive.
bool nanojs8_validate_callsign(const char *s);

// Maidenhead grid square: exactly 4 or 6 chars.
//   4-char: AA00..RR99  (first two A-R uppercase, last two 0-9)
//   6-char: AA00aa..RR99xx (additional aa..xx lowercase)
// Case insensitive on input — caller normalizes.
bool nanojs8_validate_grid(const char *s);

// Groups list: comma-separated entries, each starting with '@' followed
// by 1-15 alphanumeric chars. Empty string is valid (no groups).
// Examples: "@CQ", "@CQ,@DX", "@CQ,@CQNA,@JS8NET"
bool nanojs8_validate_groups(const char *s);

// Units: exactly "miles" or "km" (case insensitive).
bool nanojs8_validate_units(const char *s);

// Frequency: decimal MHz string (e.g. "7.078", "14.078", "144.150").
// Range: 0.1 .. 1000 MHz. Bare integer is also accepted ("7078000"
// for someone typing Hz directly).
//
// On success returns true and writes the value in Hz to *out_hz.
// On failure returns false; *out_hz is undefined.
bool nanojs8_parse_freq(const char *s, uint64_t *out_hz);

// Radio ID: 1-23 chars, alphanumeric + hyphen + underscore. The actual
// list of valid IDs is enforced in L6b.4 by the radio registry; L6b.3
// just rejects obvious garbage.
bool nanojs8_validate_radio_id(const char *s);

// L7.0: Parse a UTC time-of-day string into hour/minute/second.
// Accepts two forms (whichever is easier for the operator to type):
//   "HHMMSS"   — bare 6 digits, e.g. "142307"
//   "HH:MM:SS" — colon-separated, e.g. "14:23:07"
//
// Leading zeros required in the colon form (so "9:5:3" is invalid;
// must be "09:05:03"). The 6-digit form is rigid by definition.
//
// Hour ∈ [0,23], minute ∈ [0,59], second ∈ [0,59]. Returns true and
// writes the three values on success; false and leaves outputs
// untouched on failure.
//
// Empty string returns false (operator typed nothing — can't commit).
bool nanojs8_parse_utc(const char *s,
                       uint8_t *out_hour,
                       uint8_t *out_minute,
                       uint8_t *out_second);

// Normalization helpers. The functions in this group MUTATE the input
// in-place. Call AFTER validation on the buffer about to be committed.

// Uppercase the entire string in-place.
void nanojs8_normalize_upper(char *s);

// Maidenhead grid: uppercase the first 4 chars, lowercase the last 2
// (if present). Caller must have already validated.
void nanojs8_normalize_grid(char *s);

#ifdef __cplusplus
}
#endif
