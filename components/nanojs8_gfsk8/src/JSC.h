/**
 * @file JSC.h
 * @brief JSC word-based compression for JS8 messages.
 *
 * Rewritten from the original JSC.h.  The public interface is preserved
 * exactly — same class name, same static methods, same type aliases — but
 * the typedef names are restructured:
 *   - CodewordPair → BitwordPair  (typedef alias kept for compatibility)
 *   - Codeword     → BitVector    (typedef alias kept for compatibility)
 *   - Tuple        → JscEntry     (struct tag rename; layout identical)
 *
 * (C) 2018 Jordan Sherer <kn4crd@gmail.com> - All Rights Reserved
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ── Core types ────────────────────────────────────────────────────────────────

/// A bit vector (codeword) used for compression.
typedef std::vector<bool> BitVector;
typedef BitVector Codeword;   ///< Legacy alias.

/// A compressed codeword paired with its decoded character count.
typedef std::pair<BitVector, uint32_t> BitwordPair;
typedef BitwordPair CodewordPair;  ///< Legacy alias.

/// One entry in the JSC lookup tables.
typedef struct JscEntry {
    char const *str;   ///< NUL-terminated word string.
    int         size;  ///< Character length of str.
    int         index; ///< Position in the primary table.
} JscEntry;
typedef JscEntry Tuple;  ///< Legacy alias.

// ── JSC class ─────────────────────────────────────────────────────────────────

class JSC {
public:
    /// L7.11h.1 (Layer C2): build the sorted-by-string lookup index in
    /// PSRAM. Must be called once after nanojs8_jsc_map_init() and before
    /// any exists()/lookup()/compress() call. Idempotent — subsequent
    /// calls are no-ops.
    ///
    /// Returns true on success. On failure (PSRAM exhausted, or jsc_map
    /// not initialized) returns false and exists()/lookup() will all
    /// return false / ok=false until a subsequent successful init().
    ///
    /// Allocates ~1 MB of PSRAM (262144 × 4 bytes for the index, kept for
    /// the lifetime of the application). Sort time on ESP32-S3 @ 160 MHz
    /// with mmap'd-flash string compares is typically ~500 ms-2 s.
    static bool init(void);

    /// Encode a single word index as a variable-length bit codeword.
    static Codeword codeword(uint32_t index, bool separate, uint32_t bytesize,
                             uint32_t s, uint32_t c);

    /// Compress text to a list of (codeword, charcount) pairs.
    static std::vector<CodewordPair> compress(std::string const &text);

    /// Decompress a bit vector back to a string.
    static std::string decompress(Codeword const &bits);

    /// Return true if w has an exact-length entry; set *pIndex if so.
    static bool     exists(std::string const &w, uint32_t *pIndex);

    /// Find the longest prefix of b in the map; set *ok = false if not found.
    static uint32_t lookup(std::string const &w, bool *ok);
    static uint32_t lookup(char const *b, bool *ok);

    // ── Table sizes and data ─────────────────────────────────────────────────

    static const uint32_t size = 262144;
    static const Tuple    map[262144];
    static const Tuple    list[262144];

    static const uint32_t prefixSize = 103;
    static const Tuple    prefix[103];
};
