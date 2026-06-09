/*
 * setup_validators.cpp — per-field validators (L6b.3)
 * =====================================================
 * See setup_validators.h for the API and rationale.
 *
 * Implementation notes:
 *   - We hand-roll the validators rather than using regex to keep code
 *     size down. Each is a simple character scan.
 *   - All comparisons normalize the input to uppercase for case-
 *     insensitive matching, but do NOT mutate the buffer. Callers
 *     invoke the *_normalize_* functions before committing if they
 *     want case canonicalized in NVS.
 *   - strtod is used for frequency parsing. It's locale-aware, but on
 *     ESP-IDF the locale is always C, so '.' is the decimal separator
 *     regardless of the operator's region — matches MicroJS8.
 *
 * License: GPL-3.0
 */

#include "setup_validators.h"
#include "radio.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Returns true if c is in [A-Z] after uppercase normalization.
bool is_alpha(char c) {
    char u = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    return (u >= 'A' && u <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

// Case-insensitive equality for short strings.
bool ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 'a' + 'A') : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 'a' + 'A') : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // anonymous namespace

extern "C" bool nanojs8_validate_callsign(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 3 || n > 10) return false;

    bool has_digit = false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '/') {
            // Portable slash only allowed once and not at position 0 or
            // last char (would make the suffix empty).
            if (i == 0 || i == n - 1) return false;
            continue;
        }
        if (!is_alnum(c)) return false;
        if (is_digit(c)) has_digit = true;
    }
    if (!has_digit) return false;

    // Reject the "N0CALL" placeholder explicitly (any case). Without
    // this check the validator would happily green-light the unchanged
    // default, leaving the operator confused about why LEFT-to-HOME
    // still refuses after they "saved". Case-insensitive so n0call /
    // N0Call etc. all get rejected — on commit we'd normalize to upper
    // and they'd hit nanojs8_config_is_configured()'s rejection anyway.
    if (ieq(s, "N0CALL")) return false;

    return true;
}

extern "C" bool nanojs8_validate_grid(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n != 4 && n != 6) return false;

    // First field: two alpha A..R (case insensitive)
    for (int i = 0; i < 2; ++i) {
        char c = s[i];
        char u = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        if (u < 'A' || u > 'R') return false;
    }
    // Second field: two digits 0..9
    if (!is_digit(s[2]) || !is_digit(s[3])) return false;
    // Third field (subsquare): two alpha a..x (case insensitive)
    if (n == 6) {
        for (int i = 4; i < 6; ++i) {
            char c = s[i];
            char u = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            if (u < 'a' || u > 'x') return false;
        }
    }
    return true;
}

extern "C" bool nanojs8_validate_groups(const char *s) {
    if (!s) return false;
    // Empty groups list is valid — most ops don't subscribe to any.
    if (s[0] == '\0') return true;

    // Walk a comma-separated list. Each token must start with '@' and
    // be followed by 1-15 alphanumeric chars.
    const char *p = s;
    while (*p) {
        if (*p != '@') return false;
        ++p;
        int tok_len = 0;
        while (*p && *p != ',') {
            if (!is_alnum(*p)) return false;
            ++p;
            if (++tok_len > 15) return false;
        }
        if (tok_len == 0) return false;   // "@" with no name
        if (*p == ',') ++p;
        else if (*p == '\0') break;
    }
    return true;
}

extern "C" bool nanojs8_validate_units(const char *s) {
    if (!s) return false;
    return ieq(s, "miles") || ieq(s, "km");
}

extern "C" bool nanojs8_parse_freq(const char *s, uint64_t *out_hz) {
    if (!s || !out_hz) return false;
    if (s[0] == '\0') return false;

    // Trim trailing whitespace (the editor doesn't produce it but
    // someone could paste "7.078 ")
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) --n;
    if (n == 0) return false;

    // Make a local NUL-terminated copy so we can pass to strtod.
    char buf[24];
    if (n >= sizeof(buf)) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';

    char *end = nullptr;
    double value = strtod(buf, &end);
    if (end == buf) return false;        // no digits parsed
    if (*end != '\0') return false;       // junk after number
    if (value <= 0.0) return false;

    // Heuristic: if the value is < 10000 we treat it as MHz; otherwise
    // we treat it as Hz already. Splits the ambiguity:
    //   "7.078"     → 7.078 MHz   → 7,078,000 Hz
    //   "7078000"   → 7,078,000 Hz (already in Hz)
    //   "144.150"   → 144.150 MHz → 144,150,000 Hz
    //   "440"       → 440 MHz     → 440,000,000 Hz
    uint64_t hz;
    if (value < 10000.0) {
        // MHz input
        hz = (uint64_t)(value * 1000000.0 + 0.5);
    } else {
        // Hz input
        hz = (uint64_t)(value + 0.5);
    }
    // Sanity range for amateur use: 1.8 MHz (160m) .. 1.3 GHz (23cm)
    if (hz < 1800000ULL)       return false;
    if (hz > 1300000000ULL)    return false;

    *out_hz = hz;
    return true;
}

extern "C" bool nanojs8_validate_radio_id(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 1 || n > 23) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!is_alnum(c) && c != '-' && c != '_') return false;
    }
    // L6b.4: tighten to require the id be one of the registered radio
    // profiles. The character class check above stays so we give the
    // operator graceful "you typed an illegal char" red BEFORE the
    // longer "this isn't a known radio" red kicks in. Both end at red,
    // but typing one char at a time the char-class check goes red on
    // bad chars while the registry check stays accepting-ish until a
    // full known id is typed.
    if (nanojs8_radio_lookup(s) == nullptr) return false;
    return true;
}

// L7.0: Parse UTC HH:MM:SS or HHMMSS. Defensive against pathological
// inputs — every character is bounds-checked, no strtol on user input.
// Returns false on any deviation from the two accepted forms.
extern "C" bool nanojs8_parse_utc(const char *s,
                                  uint8_t *out_hour,
                                  uint8_t *out_minute,
                                  uint8_t *out_second) {
    if (!s || !*s) return false;
    size_t len = strlen(s);

    int h = -1, m = -1, sec = -1;

    if (len == 6) {
        // Bare digits "HHMMSS"
        for (size_t i = 0; i < 6; ++i) {
            if (!is_digit(s[i])) return false;
        }
        h   = (s[0] - '0') * 10 + (s[1] - '0');
        m   = (s[2] - '0') * 10 + (s[3] - '0');
        sec = (s[4] - '0') * 10 + (s[5] - '0');
    } else if (len == 8) {
        // Colon form "HH:MM:SS"
        if (s[2] != ':' || s[5] != ':') return false;
        if (!is_digit(s[0]) || !is_digit(s[1]) ||
            !is_digit(s[3]) || !is_digit(s[4]) ||
            !is_digit(s[6]) || !is_digit(s[7])) return false;
        h   = (s[0] - '0') * 10 + (s[1] - '0');
        m   = (s[3] - '0') * 10 + (s[4] - '0');
        sec = (s[6] - '0') * 10 + (s[7] - '0');
    } else {
        // Any other length is invalid — no partial-edit acceptance.
        // This also rejects empty strings up front (len 0).
        return false;
    }

    if (h < 0 || h > 23) return false;
    if (m < 0 || m > 59) return false;
    if (sec < 0 || sec > 59) return false;

    if (out_hour)   *out_hour   = (uint8_t)h;
    if (out_minute) *out_minute = (uint8_t)m;
    if (out_second) *out_second = (uint8_t)sec;
    return true;
}

extern "C" void nanojs8_normalize_upper(char *s) {
    if (!s) return;
    for (; *s; ++s) {
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
    }
}

extern "C" void nanojs8_normalize_grid(char *s) {
    if (!s) return;
    // L6b.4-hotfix2: was first-4-upper / last-2-lower (Maidenhead spec
    // form). Changed to ALL UPPER because JS8Call transmits the
    // subsquare as caps on the wire regardless, and uppercase-only is
    // friendlier on the T-Deck keyboard (no shift gymnastics).
    for (size_t i = 0; *s; ++s, ++i) {
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
    }
}
