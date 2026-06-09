/**
 * @file JSC.cpp
 * @brief JSC word-based compression implementation.
 *
 * Rewritten from the original JSC.cpp.  The algorithms and all numeric
 * constants are identical; the rewrite uses renamed local variables and
 * a different internal cache name (s_wordCache instead of LOOKUP_CACHE).
 *
 * (C) 2018 Jordan Sherer <kn4crd@gmail.com> - All Rights Reserved
 *
 * ─────────────────────────────────────────────────────────────────────
 * NanoJS8 modifications (L7.4b, GPL-3.0):
 *   1. decompress() now reads its dictionary word via the host project's
 *      memory-mapped JSC partition (nanojs8_jsc_map_word) instead of
 *      the in-binary JSC::map[] array. This keeps ~5 MB of dictionary
 *      data out of the app partition. See components/nanojs8_jsc_map.
 *   2. The TX-side functions compress(), codeword(), exists(), and
 *      both lookup() overloads are stubbed to return empty/false
 *      because NanoJS8 v0.7 is RX-only. Eliminating real bodies for
 *      these removes ODR-use of JSC::list[] and JSC::map[], so neither
 *      array needs a real definition — saving ~6 MB of stub flash.
 *      When TX is added (future layer) we'll restore real bodies and
 *      back the lookups with an mmap'd JSC::list partition.
 * ─────────────────────────────────────────────────────────────────────
 */

#include "JSC.h"
#include "Varicode.h"
#include "jsc_map.h"        // L7.4b: memory-mapped JSC dictionary accessor

#include <algorithm>        // L7.11h.1: std::sort for sorted-by-string index
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "esp_err.h"        // L7.11h.1: esp_err_t for C-callable wrapper
#include "esp_log.h"        // L7.11h.1: init progress + timing logs
#include "esp_timer.h"      // L7.11h.1: esp_timer_get_time for sort timing
#include "esp_heap_caps.h"  // L7.11h.1: heap_caps_malloc(MALLOC_CAP_SPIRAM)
#include "freertos/FreeRTOS.h"  // L7.11h.1-fix1: taskYIELD during sort
#include "freertos/task.h"      // L7.11h.1-fix1: taskYIELD during sort

// ──────────────────────────────────────────────────────────────────────────────
// File-static state for the C2 sorted-by-string lookup index.
//
// Hoisted up here (rather than co-located with init() in the C2 section below)
// because Layer C3's compress() — which appears earlier in this file — needs
// to read s_jscInitDone to refuse work when the index isn't ready. Without
// this forward placement, compress() fails to compile against statics that
// aren't yet declared in source order.
//
// The actual values are assigned/used in the init/exists/lookup functions of
// the C2 block.
// ──────────────────────────────────────────────────────────────────────────────

static const char *kJscTag = "JSC";

static uint32_t  *s_sortedIndex   = nullptr;   ///< PSRAM, N elements
static bool       s_jscInitDone   = false;
static uint32_t   s_maxWordLen    = 0;         ///< Longest dict entry's strlen

// NanoJS8 L7.4b: per-process cache removed. Original implementation
// used it to memoize lookup() results; lookup() is now stubbed for
// our RX-only build, so the cache is dead.

// ── Codeword generation ───────────────────────────────────────────────────────
//
// L7.11h.0 (Layer C1 of full JSC compress restoration): real implementation.
// Pure bit-math inverse of decompress() above. Does NOT touch the dictionary —
// just maps a numeric index to its variable-length bit codeword. The dictionary-
// lookup half (exists/lookup/compress) is restored in later C-layers.
//
// Encoding scheme (must invert decompress() exactly):
//
//   Let b = bytesize (4), s = separator threshold (7), c = 2^b - s (9).
//
//   The codeword for index `idx` consists of:
//     1. `depth` "prefix" nibbles (each value 7..15, i.e. ≥ s on the wire)
//     2. one "final" nibble (value 0..6, i.e. < s on the wire — signals end)
//     3. one separator bit (1 = a word-space follows, 0 = next codeword butts up)
//
//   `depth` is chosen such that base[depth] ≤ idx < base[depth+1], where:
//     base[0] = 0
//     base[d] = base[d-1] + s * c^(d-1)
//
//   Within a given depth, offset = idx - base[depth], split as:
//     accum = offset / s    (a `depth`-digit number in base c)
//     final = offset mod s  (one nibble, 0..s-1)
//
//   The prefix nibbles encode `accum` in base c, MSB-digit first, with each
//   digit `d` written on the wire as `d + s` (so it falls in 7..15 and is
//   distinguishable from the final terminator).
//
// Bit ordering on the wire is MSB-first within each nibble (matches
// Varicode::bitsToInt which reads `v = (v << 1) + bit`).
//
// Verified by the round-trip self-test in js8_encode.cpp (Subtest JSC) at
// every depth boundary including idx=0 (min, depth 0), idx=262143 (max for
// our 262144-entry dictionary, depth 5), and the separator-bit case.

Codeword JSC::codeword(uint32_t index,
                       bool     separate,
                       uint32_t bytesize,
                       uint32_t s,
                       uint32_t c)
{
    Codeword bits;

    // Compute base[] for the (s, c) parameters. Mirrors decompress; we
    // recompute locally rather than share a global so this function stays
    // a pure leaf (no init dependencies, no shared state). The original
    // JSC API allows callers to override s/c per-call (rarely used in
    // practice — the wire uses 7/9 — but we honour it for compatibility).
    //
    // depth 0..7 is the representable range; with s=7, c=9 the max
    // representable index is base[7] + s*c^7 - 1 ≈ 37.6 million, well
    // beyond our 262144-entry dictionary so we never saturate.
    uint32_t base[8];
    base[0] = 0;
    {
        uint64_t pow_c = 1;
        for (int d = 1; d < 8; ++d) {
            base[d] = base[d - 1] + (uint32_t)(s * pow_c);
            pow_c *= c;
        }
    }

    // Determine depth: smallest d such that base[d+1] > index. Clamp at 7
    // for safety — callers should never feed indices beyond the dictionary
    // anyway (compress() will guard upstream).
    uint32_t depth = 0;
    while (depth < 7 && index >= base[depth + 1]) {
        ++depth;
    }

    // Split into prefix-accumulator and final terminating nibble.
    const uint32_t offset = index - base[depth];
    const uint32_t accum  = offset / s;     // depth digits in base c
    const uint32_t finalN = offset % s;     // 0..s-1, < s on the wire

    // Emit `depth` prefix nibbles, MSB digit first. Each digit `d` is
    // written on the wire as `d + s` (so the receiver sees a value ≥ s
    // and knows this is a continuation, not a terminator).
    //
    // To extract the MSB digit first we iterate from position depth-1
    // down to 0, dividing accum by c^position and taking mod c.
    for (int pos = (int)depth - 1; pos >= 0; --pos) {
        uint32_t divisor = 1;
        for (int e = 0; e < pos; ++e) divisor *= c;
        const uint32_t digit  = (accum / divisor) % c;   // 0..c-1
        const uint32_t nibble = digit + s;               // s..s+c-1 (= 7..15)

        // Emit `bytesize` bits of `nibble`, MSB first (matches bitsToInt).
        for (int b = (int)bytesize - 1; b >= 0; --b) {
            bits.push_back(((nibble >> b) & 1u) != 0);
        }
    }

    // Emit the final terminating nibble (0..s-1), MSB first.
    for (int b = (int)bytesize - 1; b >= 0; --b) {
        bits.push_back(((finalN >> b) & 1u) != 0);
    }

    // Emit the separator bit. The receiver's decompress reads this bit
    // unconditionally and treats a 1 as "a word-space follows this
    // codeword". If there are no more bits after this one (i.e., this is
    // the last codeword in the frame), decompress's `total - pos > 0`
    // guard makes the value harmless.
    bits.push_back(separate);

    return bits;
}

// ── Compression ───────────────────────────────────────────────────────────────
// NanoJS8 L7.4b: stubbed (TX-only function, RX build doesn't call this).
// Original body accessed JSC::map[].size and called lookup(); removing the
// body removes ODR-use of both JSC::map[] and the lookup chain that uses
// JSC::list[]/JSC::prefix[]. Saves ~6 MB of stub flash.

// ── L7.11h.2 (Layer C3): real compress() implementation ───────────────────────
//
// Walks the input string, at each position calling JSC::lookup(char*) to find
// the LONGEST dictionary entry that's a prefix of the remaining input. The
// matched word becomes a codeword (via JSC::codeword from C1). If the very
// next character after the matched word is ASCII space (0x20), the codeword's
// separator bit is set — this tells decompress to emit a single " " after the
// word and effectively absorbs the input space into the codeword for zero
// extra bits beyond the always-present 1-bit separator.
//
// Each emitted CodewordPair has:
//   - .first  = codeword bits (depth prefix nibbles + final nibble + sep bit)
//   - .second = chars consumed from the input (mlen, +1 if separator set)
//
// If no dict entry matches at some position (lookup returns ok=false), we
// stop and return what we have so far. The caller (packCompressedMessage)
// records how many chars made it into the frame; packDataMessage compares
// against Huffman's char count and picks the larger. fix6 sanitization
// upstream catches chars that aren't in the JS8 Huffman alphabet (since
// huff is all-or-nothing) — for the compressed path, missing chars just
// truncate the consumed range cleanly.
//
// Wire compatibility: this produces bits that stock JS8Call's decompress
// will reconstruct exactly, because we use the same dict (vendored jsc_map)
// and our codeword() inverts decompress() bit-for-bit (verified C1).

std::vector<CodewordPair> JSC::compress(std::string const &text)
{
    std::vector<CodewordPair> result;

    if (!s_jscInitDone || text.empty()) {
        return result;
    }

    const char *const begin = text.c_str();
    const char *const end   = begin + text.size();
    const char *      cur   = begin;

    while (cur < end) {
        bool ok = false;
        const uint32_t idx = JSC::lookup(cur, &ok);
        if (!ok) {
            // No dict prefix matches at this position. Stop here; caller
            // falls back to Huffman for the un-consumed remainder, or
            // picks Huffman over compressed entirely if huff packs more.
            break;
        }

        const char  *matched = nanojs8_jsc_map_word(idx);
        if (matched == nullptr) {
            // Shouldn't happen — lookup returned ok with a valid index.
            // Defensive: bail rather than risk an infinite loop.
            break;
        }
        const size_t mlen = std::strlen(matched);
        if (mlen == 0) {
            // Empty-string dict entry would cause us to never advance.
            // No real dict should contain one, but guard anyway.
            break;
        }

        // Check: is the very next input character a space? If so, set
        // the separator bit and consume that space — decompress will
        // emit it. Always check end-bound first to avoid OOB read.
        const char *next = cur + mlen;
        const bool hasSpaceAfter = (next < end && *next == ' ');

        // Emit codeword. Parameters (b=4, s=7, c=9) match decompress()
        // and must NOT be changed — they're baked into the wire format.
        const Codeword bits = JSC::codeword(idx, hasSpaceAfter,
                                            /*bytesize=*/4,
                                            /*s=*/7,
                                            /*c=*/9);

        const uint32_t consumed =
            static_cast<uint32_t>(mlen) + (hasSpaceAfter ? 1u : 0u);

        result.push_back({bits, consumed});
        cur += consumed;
    }

    return result;
}

// ── Decompression ─────────────────────────────────────────────────────────────

std::string JSC::decompress(Codeword const &bitvec)
{
    constexpr uint32_t b = 4;
    constexpr uint32_t s = 7;
    const     uint32_t c = static_cast<uint32_t>(std::pow(2, b)) - s;

    // Pre-compute base offsets for each encoding depth (0..7).
    uint32_t base[8];
    base[0] = 0;
    base[1] = s;
    base[2] = base[1] + s * c;
    base[3] = base[2] + s * c * c;
    base[4] = base[3] + s * c * c * c;
    base[5] = base[4] + s * c * c * c * c;
    base[6] = base[5] + s * c * c * c * c * c;
    base[7] = base[6] + s * c * c * c * c * c * c;

    // Decode the bit vector into a list of 4-bit bytes plus separator positions.
    std::vector<uint64_t> nibbles;
    std::vector<uint32_t> separatorPositions;

    int pos   = 0;
    int total = static_cast<int>(bitvec.size());
    while (pos < total) {
        Codeword chunk(bitvec.begin() + pos,
                       bitvec.begin() + std::min(pos + 4, total));
        if (static_cast<int>(chunk.size()) != 4) break;

        uint64_t nibble = Varicode::bitsToInt(chunk);
        nibbles.push_back(nibble);
        pos += 4;

        if (nibble < s) {
            if (total - pos > 0 && bitvec[pos])
                separatorPositions.push_back(
                    static_cast<uint32_t>(nibbles.size()) - 1);
            pos += 1;
        }
    }

    // Reconstruct words from the nibble stream.
    std::vector<std::string> parts;
    uint32_t start = 0;
    while (start < static_cast<uint32_t>(nibbles.size())) {
        uint32_t depth = 0;
        uint32_t accum = 0;

        while (start + depth < static_cast<uint32_t>(nibbles.size()) &&
               nibbles[start + depth] >= s)
        {
            accum = accum * c + static_cast<uint32_t>(nibbles[start + depth] - s);
            ++depth;
        }
        if (accum >= JSC::size) break;
        if (start + depth >= static_cast<uint32_t>(nibbles.size())) break;

        uint32_t idx = accum * s +
                       static_cast<uint32_t>(nibbles[start + depth]) +
                       base[depth];
        if (idx >= JSC::size) break;

        parts.push_back(std::string(nanojs8_jsc_map_word(idx)));

        if (!separatorPositions.empty() &&
            separatorPositions.front() == start + depth)
        {
            parts.push_back(" ");
            separatorPositions.erase(separatorPositions.begin());
        }

        start += depth + 1;
    }

    std::string out;
    for (auto const &p : parts) out += p;
    return out;
}

// ── L7.11h.1 (Layer C2): sorted-by-string lookup index ────────────────────────
//
// The dictionary on disk (mmap'd `jsc_map` partition) is ordered by codeword
// frequency, not by string content — so [0]='E', [25]='+', [262143]='ROSIDS'
// per the boot-time sanity check. That ordering is fine for decompress (which
// indexes by numeric codeword) but useless for compress, which needs the
// inverse: given a string, find its numeric index.
//
// We build an auxiliary 262144-entry array of dict-indices, sorted such that
// nanojs8_jsc_map_word(s_sortedIndex[i]) is lexicographically increasing in i.
// Then exists() / lookup() are O(log N) binary searches against this index.
//
// Memory: 262144 × 4 B = 1 MB in PSRAM. Allocated once at boot via
// nanojs8_jsc_init(), kept for the lifetime of the application. The dict
// strings themselves are NOT copied — they stay in the mmap'd flash region;
// the index stores only uint32_t indices that resolve to mmap pointers via
// nanojs8_jsc_map_word().
//
// Sort time is O(N log N) string compares against mmap'd flash; on the
// ESP32-S3 @ 160 MHz this typically runs ~500 ms-2 s. One-time cost at boot.
//
// (File-static state — s_sortedIndex, s_jscInitDone, s_maxWordLen — and the
// kJscTag log tag are declared at the top of this file so that the C3
// compress() implementation, which appears earlier in source order, can
// reference them.)

bool JSC::init(void)
{
    if (s_jscInitDone) {
        return true;  // Idempotent: subsequent calls are no-ops.
    }

    // The mmap must be live before we can read dict words. If
    // nanojs8_jsc_map_init() wasn't called (or failed), every
    // nanojs8_jsc_map_word() returns NULL, which would crash our sort
    // comparator on strcmp(NULL, ...). Fail-fast with a clear log.
    if (nanojs8_jsc_map_word(0) == nullptr) {
        ESP_LOGE(kJscTag,
            "init: jsc_map partition not loaded — "
            "call nanojs8_jsc_map_init() before JSC::init()");
        return false;
    }

    constexpr uint32_t N = JSC::size;  // 262144

    // ──────────────────────────────────────────────────────────────────
    // L7.11h.1-fix1 — PSRAM-cached pool + yielding sort.
    //
    // Initial L7.11h.1 sort took 15.2 s and triggered the IDLE0 task
    // watchdog. Root cause: nanojs8_jsc_map_word() returns a pointer into
    // the mmap'd flash region (~0x3c8f0000). The 5M random-access string
    // compares during std::sort thrash the 32 KB MMU/flash cache, making
    // each strcmp() ~3 µs. Plus main task hogs CPU 0 the entire time, so
    // IDLE0 never runs to feed its watchdog.
    //
    // Fix: copy the dict strings to a PSRAM heap pool, build a temp
    // (str, idx) array, sort against PSRAM (cached more effectively),
    // then extract the sorted indices into the permanent 1 MB index.
    // Comparator yields every ~1M comparisons so IDLE0 can reset the
    // task watchdog.
    //
    // Peak PSRAM during init: 1 MB perm + ~1.9 MB str pool + ~2 MB temp
    // SortEntry array = ~5 MB. Free PSRAM at this point in boot is
    // ~8.4 MB (rx_audio's 1.4 MB ring is allocated AFTER us). The temp
    // arrays are freed before we return, leaving only the permanent
    // 1 MB index. Net memory footprint is identical to the original
    // implementation; only the transient peak is higher.
    // ──────────────────────────────────────────────────────────────────

    // ── Phase 1: scan dict for total size + longest word ─────────────
    //
    // One pass over all 262144 entries. Establishes the byte budget for
    // the string pool and records max word length for lookup() bounding.
    // Yields every 64K entries — at this point we're reading flash
    // sequentially per entry so it's fast, but we still want to feed
    // IDLE0 occasionally.
    size_t   total_str_bytes = 0;
    uint32_t max_len_seen    = 0;
    for (uint32_t i = 0; i < N; ++i) {
        const char *s = nanojs8_jsc_map_word(i);
        if (s != nullptr) {
            const size_t len = std::strlen(s);
            total_str_bytes += len + 1;        // +1 for NUL
            if (len > max_len_seen) max_len_seen = (uint32_t)len;
        } else {
            total_str_bytes += 1;              // empty string + NUL
        }
        if ((i & 0xFFFFu) == 0xFFFFu) {
            taskYIELD();
        }
    }
    s_maxWordLen = max_len_seen;

    // ── Phase 2: allocate the permanent 1 MB sorted index in PSRAM ───
    const size_t perm_bytes = (size_t)N * sizeof(uint32_t);
    s_sortedIndex = (uint32_t *)heap_caps_malloc(
        perm_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_sortedIndex == nullptr) {
        ESP_LOGE(kJscTag,
            "init: failed to allocate %u B permanent sorted index in PSRAM",
            (unsigned)perm_bytes);
        return false;
    }

    // ── Phase 3: allocate temp PSRAM string pool ─────────────────────
    char *str_pool = (char *)heap_caps_malloc(
        total_str_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (str_pool == nullptr) {
        ESP_LOGE(kJscTag,
            "init: failed to allocate %u B temp PSRAM string pool",
            (unsigned)total_str_bytes);
        heap_caps_free(s_sortedIndex);
        s_sortedIndex = nullptr;
        return false;
    }

    // ── Phase 4: allocate temp (str, idx) sort entry array ───────────
    struct SortEntry {
        const char *str;
        uint32_t    idx;
    };
    const size_t temp_bytes = (size_t)N * sizeof(SortEntry);
    SortEntry *temp = (SortEntry *)heap_caps_malloc(
        temp_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (temp == nullptr) {
        ESP_LOGE(kJscTag,
            "init: failed to allocate %u B temp SortEntry array",
            (unsigned)temp_bytes);
        heap_caps_free(str_pool);
        heap_caps_free(s_sortedIndex);
        s_sortedIndex = nullptr;
        return false;
    }

    // ── Phase 5: copy dict strings to PSRAM pool + build temp[] ──────
    //
    // Each iteration reads from mmap'd flash (~1 µs uncached) and writes
    // to PSRAM (~100 ns). Sequential access, so flash cache is more
    // effective than during random-access sort. This phase typically
    // runs in 300-600 ms.
    const int64_t t_copy_start = esp_timer_get_time();
    char *cursor = str_pool;
    for (uint32_t i = 0; i < N; ++i) {
        const char *src = nanojs8_jsc_map_word(i);
        if (src != nullptr) {
            const size_t len = std::strlen(src);
            std::memcpy(cursor, src, len);
            cursor[len] = '\0';
            temp[i].str = cursor;
            cursor += len + 1;
        } else {
            *cursor       = '\0';
            temp[i].str   = cursor;
            cursor       += 1;
        }
        temp[i].idx = i;
        if ((i & 0xFFFFu) == 0xFFFFu) {
            taskYIELD();
        }
    }
    const int64_t t_copy_end = esp_timer_get_time();

    // ── Phase 6: sort temp[] by string content (PSRAM accesses) ──────
    //
    // PSRAM has its own larger cache and faster access path than mmap'd
    // flash. Comparator yields every 1M comparisons (~5 yields total)
    // to keep IDLE0 alive. The static counter is safe — std::sort runs
    // single-threaded and init() is called only once.
    const int64_t t_sort_start = esp_timer_get_time();
    std::sort(temp, temp + N,
              [](const SortEntry &a, const SortEntry &b) {
                  static uint32_t cmp_counter = 0;
                  if ((++cmp_counter & 0x000FFFFFu) == 0) {
                      taskYIELD();
                  }
                  return std::strcmp(a.str, b.str) < 0;
              });
    const int64_t t_sort_end = esp_timer_get_time();

    // ── Phase 7: extract sorted indices into permanent index ─────────
    for (uint32_t i = 0; i < N; ++i) {
        s_sortedIndex[i] = temp[i].idx;
    }

    // ── Phase 8: free temp PSRAM allocations ─────────────────────────
    heap_caps_free(temp);
    heap_caps_free(str_pool);

    s_jscInitDone = true;
    ESP_LOGI(kJscTag,
        "init: sorted-by-string index built — "
        "copy=%lld ms, sort=%lld ms (%u entries, %u B PSRAM perm, "
        "max dict word = %u chars)",
        (long long)((t_copy_end  - t_copy_start) / 1000LL),
        (long long)((t_sort_end  - t_sort_start) / 1000LL),
        (unsigned)N, (unsigned)perm_bytes, (unsigned)max_len_seen);

    return true;
}

// ── Internal binary search ────────────────────────────────────────────────────
//
// Exact-string lookup against the sorted index. Returns true and sets *pIndex
// to the ORIGINAL dict-index (not the sorted-array position) on hit.
//
// Caller MUST have verified s_jscInitDone before calling. s_sortedIndex must
// be non-NULL and `w` must be non-NULL — we don't re-check here to keep this
// hot-path tight; the only callers (exists / lookup) guard before invoking.

static bool jscBinSearchExact(const char *w, uint32_t *pIndex)
{
    if (s_sortedIndex == nullptr || w == nullptr) {
        return false;  // Defensive fallback — shouldn't trigger in practice.
    }

    int32_t lo = 0;
    int32_t hi = (int32_t)JSC::size - 1;
    while (lo <= hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const char *m = nanojs8_jsc_map_word(s_sortedIndex[mid]);
        const int cmp = std::strcmp(m, w);
        if (cmp == 0) {
            if (pIndex) *pIndex = s_sortedIndex[mid];
            return true;
        } else if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return false;
}

// ── Existence check ───────────────────────────────────────────────────────────

bool JSC::exists(std::string const &w, uint32_t *pIndex)
{
    if (!s_jscInitDone) {
        if (pIndex) *pIndex = 0;
        return false;
    }
    return jscBinSearchExact(w.c_str(), pIndex);
}

// ── Lookup (std::string overload — exact match) ───────────────────────────────

uint32_t JSC::lookup(std::string const &w, bool *ok)
{
    uint32_t idx = 0;
    if (s_jscInitDone && jscBinSearchExact(w.c_str(), &idx)) {
        if (ok) *ok = true;
        return idx;
    }
    if (ok) *ok = false;
    return 0;
}

// ── Lookup (C-string overload — LONGEST-PREFIX match) ─────────────────────────
//
// This is the workhorse for compress() (restored in Layer C3). For input
// position `b`, returns the dict-index of the longest dictionary entry that
// is a prefix of `b`. If no entry matches even the first character of `b`,
// sets *ok = false.
//
// Algorithm: try prefix lengths from min(strlen(b), maxWordLen) down to 1,
// binary-searching the sorted index for each candidate prefix. The first hit
// is the longest match. Stack buffer is sized to maxWordLen+1; if the dict
// ever grows entries longer than 63 chars we bail out gracefully.
//
// Cost: O(L × log N) per call, where L = min(strlen(b), maxWordLen). With
// L ≈ 6 (typical max JSC word) and N = 262144, that's ~110 string-compare
// ops per longest-prefix lookup — well under a millisecond.

uint32_t JSC::lookup(char const *b, bool *ok)
{
    if (!s_jscInitDone || b == nullptr || b[0] == '\0') {
        if (ok) *ok = false;
        return 0;
    }

    const size_t blen = std::strlen(b);
    const size_t maxK = (blen < s_maxWordLen) ? blen : s_maxWordLen;

    // Stack buffer for the candidate prefix. 64 bytes is comfortable for
    // any plausible JSC dictionary entry (we've never seen one exceed ~20
    // chars). If a hypothetical future dict has longer entries the
    // sanity check below trips and we degrade to "no match" gracefully.
    char buf[64];
    if (maxK >= sizeof(buf)) {
        ESP_LOGW(kJscTag,
            "lookup(char*): max dict word length (%u) exceeds buf size "
            "(%u) — degrading to no-match",
            (unsigned)maxK, (unsigned)sizeof(buf));
        if (ok) *ok = false;
        return 0;
    }

    for (size_t k = maxK; k >= 1; --k) {
        std::memcpy(buf, b, k);
        buf[k] = '\0';
        uint32_t idx;
        if (jscBinSearchExact(buf, &idx)) {
            if (ok) *ok = true;
            return idx;
        }
    }

    if (ok) *ok = false;
    return 0;
}

// ── C-callable wrapper for main.c ─────────────────────────────────────────────
//
// main.c is C, not C++; it can't call JSC::init() directly. This thin wrapper
// translates the bool return to esp_err_t to match the rest of the app's init
// API style (mirrors nanojs8_jsc_map_init).

extern "C" esp_err_t nanojs8_jsc_init(void)
{
    return JSC::init() ? ESP_OK : ESP_ERR_NO_MEM;
}
