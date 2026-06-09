/*
 * js8_freetext.c — JS8 free-text alphabet sanitization (L7.11g.7-fix6)
 * =============================================================================
 * See js8_freetext.h for the full rationale. This translation unit is a
 * leaf — no FreeRTOS, no ESP-IDF, no heap. Safe to call from any task
 * context, including ISRs (it does no logging, no blocking).
 *
 * Implementation note: rather than a strchr() / std::set scan per char (the
 * pattern used in JS8Call's `packHuffMessage`), we use a 256-entry static
 * lookup table indexed by unsigned char. Build cost is one-time at module
 * load (constexpr-able if we needed it); lookup is O(1). Sanitization of a
 * 100-byte body is ~100 array indexings — well under one microsecond.
 *
 * Why a table instead of strchr():
 *   - O(1) per char vs O(N) per char with strchr (N=44 for our alphabet)
 *   - Branch-free per char (just a load + compare)
 *   - Reads only one cache line (256 bytes, fits in a single L1 line group)
 *   - Eliminates any case-sensitivity ambiguity (the table is pre-uppercased)
 *
 * The table is built from the EXPLICIT Huffman alphabet enumerated in
 * `nanojs8_gfsk8/src/Varicode.cpp` lines 342-385. If that table ever changes
 * upstream, this one must be updated to match — the build will not catch
 * the mismatch because there's no shared symbol between them.
 */
#include "js8_freetext.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Valid characters from the Huffman codebook. Order does not matter — this
 * is enumerated for table construction only. ALL CAPS because the codebook
 * is uppercase-only; lowercase input is uppercased before the table lookup.
 */
static const char VALID_CHARS[] =
    " "
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    ".-+?!\"/";

/*
 * 256-entry lookup table. `valid_table[c]` is non-zero iff `c` (after
 * uppercasing) is in the JS8 Huffman alphabet. Indexed by *unsigned char*
 * to defang signed-char sign extension on platforms (or compilers) where
 * `char` is signed (xtensa-esp-elf-gcc treats char as signed).
 */
static bool valid_table[256];
static bool table_initialized = false;

static void init_table_once(void) {
    if (table_initialized) {
        return;
    }
    /*
     * Mark each char from VALID_CHARS as valid. The loop runs in roughly
     * 45 iterations once per boot — no need to optimize.
     */
    for (const char *p = VALID_CHARS; *p; ++p) {
        valid_table[(unsigned char)*p] = true;
    }
    table_initialized = true;
}

/*
 * Uppercase a single ASCII byte. Only operates on lowercase letters; leaves
 * everything else (including non-ASCII) untouched. Avoids locale-dependent
 * toupper() which can misbehave with negative char values.
 */
static inline char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

size_t nanojs8_js8_freetext_sanitize(char *s) {
    if (s == NULL) {
        return 0;
    }
    init_table_once();

    size_t changed = 0;
    for (char *p = s; *p != '\0'; ++p) {
        const char orig = *p;
        char c = ascii_upper(orig);
        if (!valid_table[(unsigned char)c]) {
            /*
             * Operator preference (Jun 7 2026): replace with space. Length
             * is preserved; in the recipient's display the body just reads
             * with a non-printing gap where the bad char was.
             */
            c = ' ';
        }
        if (c != orig) {
            changed++;
            *p = c;
        }
    }
    return changed;
}

size_t nanojs8_js8_freetext_count_invalid(const char *s) {
    if (s == NULL) {
        return 0;
    }
    init_table_once();

    size_t invalid = 0;
    for (const char *p = s; *p != '\0'; ++p) {
        const char c = ascii_upper(*p);
        if (!valid_table[(unsigned char)c]) {
            invalid++;
        }
    }
    return invalid;
}
