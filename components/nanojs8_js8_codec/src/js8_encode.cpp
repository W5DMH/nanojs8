/*
 * js8_encode.cpp — L7.11a JS8 TX encoder + boot-time self-test
 * =============================================================
 * Wraps the gfsk8 + Varicode encode chain behind a C-callable API. This is
 * the first step of the L7.11 TX path: we can produce the 79-tone JS8 Normal
 * waveform symbols here, but they aren't yet modulated to PCM (L7.11b) or
 * pushed out the USB UAC TX endpoint (L7.11c).
 *
 * Pipeline for a heartbeat:
 *
 *   text + callsign  →  Varicode::packHeartbeatMessage  →  12-char frame
 *   12-char frame    →  gfsk8::encode (LDPC + tone map)  →  79 tones in [0,7]
 *
 * No PTT yet, no audio, no on-air activity. The self-test validates both
 * layers (Varicode round-trip + Costas positions in the tone stream) so a
 * regression in either path surfaces in the boot log.
 *
 * License: GPL-3.0
 *   Inherits gfsk8-modem-clean's GPL-3.0; see components/nanojs8_gfsk8.
 */

#include "js8_codec.h"
#include "gfsk8modem.h"

// gfsk8 internals — we need Varicode::packHeartbeatMessage +
// unpackHeartbeatMessage from the vendored sources. They live in the
// nanojs8_gfsk8 component's internal header. Pulling them in here keeps
// the C++ vendor namespace fully contained inside this translation unit.
#include "Varicode.h"
#include "JSC.h"          // L7.11h.0: codeword() round-trip self-test
#include "jsc_map.h"      // L7.11h.0: dictionary word lookup for verification

#include "esp_log.h"
#include "esp_timer.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "js8encode";

namespace {

// L7.11f-fix1: pack() cache.
//
// gfsk8::pack() is the expensive part of TX modulation — it runs
// std::regex inside Varicode::buildMessageFrames and takes ~1.5 s for
// a typical multi-frame message. Without caching, a 6-frame TX calls
// pack() at least 7 times (1 for text_frame_count, 6 for each
// encode_text_frame). That's ~10 s of repeated regex work.
//
// More importantly, the 2.5 s combined cost of pack()+encode+modulate
// per frame doesn't fit in the 1.5 s gap between consecutive 15 s JS8
// slots. The worker ends up missing every other slot, doubling the
// effective transmission time.
//
// The cache stores the std::vector<gfsk8::TxFrame> keyed by the
// (wire,mycall,mygrid) tuple. Lookups with a matching key are O(1).
// On key mismatch the cache is rebuilt and the old entry replaced.
//
// Thread safety: a single mutex serializes lookups + rebuilds. The
// cache is intended for the single TX worker task; concurrent
// callers would see consistent results but degraded throughput from
// the lock.
struct PackCache {
    std::string                  key;
    std::vector<gfsk8::TxFrame>  frames;
};

PackCache         s_pack_cache;
SemaphoreHandle_t s_pack_mutex   = nullptr;
bool              s_pack_initted = false;

// Lazy init for the cache mutex. Called from every public entry that
// touches the cache; idempotent.
void ensure_pack_cache_init() {
    if (s_pack_initted) return;
    s_pack_mutex   = xSemaphoreCreateMutex();
    s_pack_initted = (s_pack_mutex != nullptr);
    if (!s_pack_initted) {
        ESP_LOGE(TAG, "pack-cache: failed to create mutex; falling "
                      "back to per-call pack() (slow)");
    }
}

std::string pack_cache_key(const char *wire,
                            const char *mycall,
                            const char *mygrid) {
    std::string k;
    k.reserve(64);
    k.append(wire ? wire : "");
    k.push_back('\x1f');  // unit separator — won't appear in inputs
    k.append(mycall ? mycall : "");
    k.push_back('\x1f');
    k.append(mygrid ? mygrid : "");
    return k;
}

// Returns a pointer to the cached frames for (wire,mycall,mygrid).
// On a fresh tuple, packs once and stores. Returns nullptr only on
// allocation failure or pack() exception (which is also logged).
//
// The returned pointer is valid until the NEXT call that updates
// the cache with a different key. Callers within the TX worker hold
// the worker thread, so back-to-back per-frame calls are safe.
const std::vector<gfsk8::TxFrame>*
get_or_pack(const char *wire, const char *mycall, const char *mygrid)
{
    ensure_pack_cache_init();
    if (!s_pack_initted) {
        // Mutex creation failed — fall back: pack fresh each call.
        // Caller still gets correct results, just slower.
        try {
            s_pack_cache.frames = gfsk8::pack(
                std::string(mycall),
                std::string(mygrid),
                std::string(wire),
                gfsk8::Submode::Normal);
            return &s_pack_cache.frames;
        } catch (...) {
            return nullptr;
        }
    }

    if (xSemaphoreTake(s_pack_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "pack-cache: mutex timeout");
        return nullptr;
    }

    std::string new_key = pack_cache_key(wire, mycall, mygrid);
    if (new_key == s_pack_cache.key && !s_pack_cache.frames.empty()) {
        // Cache hit — return the existing vector pointer.
        const auto *result = &s_pack_cache.frames;
        xSemaphoreGive(s_pack_mutex);
        return result;
    }

    // Cache miss — repack.
    try {
        const int64_t t0 = esp_timer_get_time();
        s_pack_cache.frames = gfsk8::pack(
            std::string(mycall),
            std::string(mygrid),
            std::string(wire),
            gfsk8::Submode::Normal);
        s_pack_cache.key = std::move(new_key);
        const int64_t t1 = esp_timer_get_time();
        ESP_LOGI(TAG, "pack-cache: rebuilt for '%s' → %u frames in "
                      "%" PRId64 " ms",
                 wire ? wire : "(null)",
                 (unsigned)s_pack_cache.frames.size(),
                 (t1 - t0) / 1000);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "pack-cache: gfsk8::pack threw '%s' for '%s'",
                 e.what(), wire ? wire : "(null)");
        s_pack_cache.key.clear();
        s_pack_cache.frames.clear();
        xSemaphoreGive(s_pack_mutex);
        return nullptr;
    } catch (...) {
        ESP_LOGE(TAG, "pack-cache: gfsk8::pack threw unknown");
        s_pack_cache.key.clear();
        s_pack_cache.frames.clear();
        xSemaphoreGive(s_pack_mutex);
        return nullptr;
    }

    const auto *result = &s_pack_cache.frames;
    xSemaphoreGive(s_pack_mutex);
    return result;
}

}  // namespace

namespace {

/// JS8 Normal Costas sync array. Identical to the one hardcoded in
/// nanojs8_gfsk8/src/JS8.h and the JS8 spec — kept here as a local copy
/// purely for self-test verification (we deliberately don't reach into
/// the vendor source for it; that would defeat the cross-check).
constexpr uint8_t kCostasJS8Normal[7] = {4, 2, 5, 6, 1, 3, 0};

/// Tone positions where the three Costas arrays live in a JS8 Normal
/// transmission. Matches gfsk8::encode's output layout.
constexpr int kCostasOffsetStart  = 0;
constexpr int kCostasOffsetMiddle = 36;
constexpr int kCostasOffsetEnd    = 72;

} // anonymous namespace

extern "C" {

bool nanojs8_js8_encode_heartbeat(const char *text,
                                   const char *callsign,
                                   uint8_t out_tones[NANOJS8_JS8_NUM_TONES])
{
    if (!text || !callsign || !out_tones) {
        ESP_LOGW(TAG, "encode_heartbeat: NULL arg");
        return false;
    }
    if (callsign[0] == '\0') {
        ESP_LOGW(TAG, "encode_heartbeat: empty callsign");
        return false;
    }

    // ── Protocol layer: human text → 12-char JS8-alphabet frame ─────────
    std::string txt(text);
    std::string call(callsign);
    int consumed = 0;
    std::string frame;
    try {
        frame = Varicode::packHeartbeatMessage(txt, call, &consumed);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "encode_heartbeat: pack threw: %s", e.what());
        return false;
    } catch (...) {
        ESP_LOGE(TAG, "encode_heartbeat: pack threw unknown exception");
        return false;
    }
    if (frame.size() != 12) {
        // Pack returns empty string on regex mismatch or invalid callsign.
        ESP_LOGW(TAG, "encode_heartbeat: pack produced %u-char frame "
                      "(expected 12) — text='%s' call='%s'",
                 (unsigned)frame.size(), text, callsign);
        return false;
    }

    // ── Physical layer: 12-char frame → 79 8-FSK tones ──────────────────
    std::vector<int> tones;
    bool ok = false;
    try {
        ok = gfsk8::encode(gfsk8::Submode::Normal,
                            static_cast<int>(Varicode::FrameHeartbeat),
                            frame, tones);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "encode_heartbeat: encode threw: %s", e.what());
        return false;
    } catch (...) {
        ESP_LOGE(TAG, "encode_heartbeat: encode threw unknown exception");
        return false;
    }
    if (!ok || tones.size() != NANOJS8_JS8_NUM_TONES) {
        ESP_LOGE(TAG, "encode_heartbeat: encode returned ok=%d size=%u",
                 (int)ok, (unsigned)tones.size());
        return false;
    }

    // Convert int tones to uint8_t while validating the [0,7] range.
    for (int i = 0; i < NANOJS8_JS8_NUM_TONES; ++i) {
        const int t = tones[i];
        if (t < 0 || t > 7) {
            ESP_LOGE(TAG, "encode_heartbeat: tone[%d]=%d out of [0,7]", i, t);
            return false;
        }
        out_tones[i] = static_cast<uint8_t>(t);
    }
    return true;
}

bool nanojs8_js8_encode_text(const char *text,
                              const char *mycall,
                              const char *mygrid,
                              uint8_t out_tones[NANOJS8_JS8_NUM_TONES])
{
    // L7.11f: thin wrapper around the frame-indexed primitive. Always
    // encodes frame 0; callers that need multi-frame TX should use
    // nanojs8_js8_text_frame_count + nanojs8_js8_encode_text_frame
    // directly to iterate.
    return nanojs8_js8_encode_text_frame(text, mycall, mygrid,
                                          0, out_tones);
}

size_t nanojs8_js8_text_frame_count(const char *text,
                                     const char *mycall,
                                     const char *mygrid)
{
    if (!text || !mycall || !mygrid || *mycall == '\0') {
        return 0;
    }
    // L7.11f-fix1: route through the cache. First call for a given
    // (text,call,grid) does the actual pack; subsequent calls are
    // O(1) hash compare + vector size read.
    const std::vector<gfsk8::TxFrame> *frames =
        get_or_pack(text, mycall, mygrid);
    if (!frames) {
        ESP_LOGE(TAG, "text_frame_count: pack failed for '%s'", text);
        return 0;
    }
    return frames->size();
}

bool nanojs8_js8_encode_text_frame(const char *text,
                                    const char *mycall,
                                    const char *mygrid,
                                    size_t frame_index,
                                    uint8_t out_tones[NANOJS8_JS8_NUM_TONES])
{
    // L7.11f-fix1: route the pack() call through the cache, so a
    // sequential 0..N-1 iteration only pays the regex/pack cost
    // once. Per-frame work after the first is just gfsk8::encode
    // (the LDPC + tone-map) which is fast (~50 ms).
    if (!text || !mycall || !mygrid || !out_tones) {
        ESP_LOGE(TAG, "encode_text_frame: null argument(s)");
        return false;
    }
    if (*mycall == '\0') {
        ESP_LOGE(TAG, "encode_text_frame: empty mycall");
        return false;
    }

    const std::vector<gfsk8::TxFrame> *frames =
        get_or_pack(text, mycall, mygrid);
    if (!frames) {
        ESP_LOGE(TAG, "encode_text_frame: pack failed for '%s'", text);
        return false;
    }

    if (frames->empty()) {
        ESP_LOGE(TAG, "encode_text_frame: pack returned 0 frames "
                      "for '%s'", text);
        return false;
    }
    if (frame_index >= frames->size()) {
        ESP_LOGE(TAG, "encode_text_frame: frame_index=%u >= total=%u "
                      "for '%s'",
                 (unsigned)frame_index,
                 (unsigned)frames->size(), text);
        return false;
    }

    try {
        const gfsk8::TxFrame &frame = (*frames)[frame_index];
        if (frame.payload.size() != 12) {
            ESP_LOGE(TAG, "encode_text_frame: frame %u payload size "
                          "%u != 12",
                     (unsigned)frame_index,
                     (unsigned)frame.payload.size());
            return false;
        }

        std::vector<int> tones;
        const bool ok = gfsk8::encode(
            gfsk8::Submode::Normal,
            frame.frameType,
            std::string_view(frame.payload),
            tones);
        if (!ok) {
            ESP_LOGE(TAG, "encode_text_frame: gfsk8::encode failed "
                          "(frameType=%d, frame %u)",
                     frame.frameType, (unsigned)frame_index);
            return false;
        }
        if (tones.size() != NANOJS8_JS8_NUM_TONES) {
            ESP_LOGE(TAG, "encode_text_frame: frame %u produced %u "
                          "tones, expected %d",
                     (unsigned)frame_index,
                     (unsigned)tones.size(),
                     NANOJS8_JS8_NUM_TONES);
            return false;
        }

        for (int i = 0; i < NANOJS8_JS8_NUM_TONES; ++i) {
            const int t = tones[i];
            if (t < 0 || t > 7) {
                ESP_LOGE(TAG, "encode_text_frame: tone[%d]=%d out of "
                              "[0,7] in frame %u",
                         i, t, (unsigned)frame_index);
                return false;
            }
            out_tones[i] = static_cast<uint8_t>(t);
        }
        return true;
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "encode_text_frame: std::exception '%s' for "
                      "'%s' (frame %u)",
                 e.what(), text, (unsigned)frame_index);
        return false;
    } catch (...) {
        ESP_LOGE(TAG, "encode_text_frame: unknown exception for '%s' "
                      "(frame %u)", text, (unsigned)frame_index);
        return false;
    }
}

bool nanojs8_js8_encode_self_test(void)
{
    ESP_LOGI(TAG, "Self-test: starting (Varicode round-trip + Costas check)");

    // ── Subtest A: Varicode pack/unpack round-trip ──────────────────────
    // Pick a heartbeat that exercises grid encoding too. EN83 is the
    // operator's grid; W5DMH is a real-world example call (matches the
    // configured station so any future on-air test uses the same data).
    const char *test_text = "HB EN83";
    const char *test_call = "W5DMH";

    {
        std::string txt(test_text);
        std::string call(test_call);
        int consumed = 0;
        std::string frame;
        try {
            frame = Varicode::packHeartbeatMessage(txt, call, &consumed);
        } catch (...) {
            ESP_LOGE(TAG, "Self-test A: pack threw");
            return false;
        }
        if (frame.size() != 12) {
            ESP_LOGE(TAG, "Self-test A: pack produced %u-char frame, want 12",
                     (unsigned)frame.size());
            return false;
        }

        // Round-trip via unpackHeartbeatMessage. We don't need to assert on
        // the exact unpacked text (the unpacker may normalize whitespace /
        // case) — what we need is that the unpacker *recognizes* the frame
        // as a heartbeat. That alone proves the framing bits are intact.
        uint8_t  frame_type = 0;
        bool     is_alt     = false;
        uint8_t  bits3      = 0;
        std::vector<std::string> unpacked;
        try {
            unpacked = Varicode::unpackHeartbeatMessage(frame, &frame_type,
                                                         &is_alt, &bits3);
        } catch (...) {
            ESP_LOGE(TAG, "Self-test A: unpack threw");
            return false;
        }
        if (unpacked.empty()) {
            ESP_LOGE(TAG, "Self-test A: unpack returned empty — frame type "
                          "or callsign packing broken");
            return false;
        }
        if (is_alt) {
            ESP_LOGE(TAG, "Self-test A: 'HB' decoded as CQ (alt=true)");
            return false;
        }
        ESP_LOGI(TAG, "Self-test A: PASS — pack/unpack round-trip OK "
                      "(unpacked %u tokens)", (unsigned)unpacked.size());
    }

    // ── Subtest JSC (L7.11h.0, Layer C1 of full JSC compress restoration) ─
    //
    // Round-trips JSC::codeword() through the existing-and-working
    // JSC::decompress(). If the bit-math in codeword() is correct, then for
    // every valid dictionary index, encoding it and decoding it must yield
    // the original word. This catches off-by-one in depth selection, wrong
    // base[] computation, swapped MSB/LSB nibble ordering, and separator-bit
    // mistakes — every class of bug that would silently emit garbage on the
    // air.
    //
    // The test sweeps both endpoints of every depth band (depth 0..5 covers
    // our 262144-entry dictionary) plus the absolute extrema. Includes a
    // multi-codeword test with a separator bit to verify the inter-word
    // space machinery.
    //
    // FAIL-STOP: any mismatch returns false → boot aborts, no TX path armed.
    // This is the entire purpose of Layer C1 — prove the bit math BEFORE we
    // hook compress() into the live encoder in later layers.
    {
        struct JscRoundTripCase {
            uint32_t    idx;
            const char *note;
        };
        // Depth boundaries computed from base[d] = s*(c^(d-1)) cumulative:
        //   base[0]=0  base[1]=7  base[2]=70  base[3]=637
        //   base[4]=5740  base[5]=51667  base[6]=465010
        // We test min/max of each occupied depth (0..5) and the absolute
        // dictionary max. idx 25 is also tested because the boot log
        // sanity-prints it as '+' so any failure there is operator-visible.
        static const JscRoundTripCase cases[] = {
            {     0u, "depth 0 min"           },
            {     6u, "depth 0 max (s-1)"     },
            {     7u, "depth 1 min"           },
            {    25u, "depth 1 mid ('+' ref)" },
            {    69u, "depth 1 max"           },
            {    70u, "depth 2 min"           },
            {   636u, "depth 2 max"           },
            {   637u, "depth 3 min"           },
            {  5739u, "depth 3 max"           },
            {  5740u, "depth 4 min"           },
            { 51666u, "depth 4 max"           },
            { 51667u, "depth 5 min"           },
            {100000u, "depth 5 mid"           },
            {262143u, "depth 5 max (dict end)"},
        };

        for (const auto &tc : cases) {
            // Encode.
            Codeword bits;
            try {
                bits = JSC::codeword(tc.idx, /*separate=*/false,
                                     /*bytesize=*/4, /*s=*/7, /*c=*/9);
            } catch (...) {
                ESP_LOGE(TAG,
                    "Self-test JSC: codeword() threw for idx=%u (%s)",
                    (unsigned)tc.idx, tc.note);
                return false;
            }
            if (bits.empty()) {
                ESP_LOGE(TAG,
                    "Self-test JSC: codeword() returned empty for "
                    "idx=%u (%s)", (unsigned)tc.idx, tc.note);
                return false;
            }

            // Expected bit-length: (depth+1)*4 + 1 separator.
            // depth 0 → 5 bits, depth 5 → 25 bits. Sanity check.
            if (bits.size() < 5 || bits.size() > 33) {
                ESP_LOGE(TAG,
                    "Self-test JSC: codeword(idx=%u, %s) produced "
                    "%zu bits — out of expected range [5,33]",
                    (unsigned)tc.idx, tc.note, bits.size());
                return false;
            }

            // Decode through the proven decompress path.
            std::string decoded;
            try {
                decoded = JSC::decompress(bits);
            } catch (...) {
                ESP_LOGE(TAG,
                    "Self-test JSC: decompress() threw on codeword "
                    "for idx=%u (%s)", (unsigned)tc.idx, tc.note);
                return false;
            }

            // Compare to the dictionary entry at that index.
            const char *expected = nanojs8_jsc_map_word(tc.idx);
            if (expected == nullptr) {
                ESP_LOGE(TAG,
                    "Self-test JSC: jsc_map_word(%u) returned NULL — "
                    "is the jsc_map partition flashed?",
                    (unsigned)tc.idx);
                return false;
            }
            if (decoded != expected) {
                ESP_LOGE(TAG,
                    "Self-test JSC FAIL: idx=%u (%s) — "
                    "expected='%s' got='%s' bits=%zu",
                    (unsigned)tc.idx, tc.note,
                    expected, decoded.c_str(), bits.size());
                return false;
            }
        }

        // Separator-bit test: codeword(0, separate=true) then
        // codeword(25, separate=false) should decompress to "<word[0]> <word[25]>".
        // With the boot-log sanity values that's "E +" — easily visible.
        {
            Codeword combined;
            const Codeword a =
                JSC::codeword(0u, /*separate=*/true, 4u, 7u, 9u);
            const Codeword b =
                JSC::codeword(25u, /*separate=*/false, 4u, 7u, 9u);
            combined.insert(combined.end(), a.begin(), a.end());
            combined.insert(combined.end(), b.begin(), b.end());

            const std::string decoded = JSC::decompress(combined);

            std::string expected;
            const char *w0  = nanojs8_jsc_map_word(0u);
            const char *w25 = nanojs8_jsc_map_word(25u);
            if (w0 == nullptr || w25 == nullptr) {
                ESP_LOGE(TAG, "Self-test JSC: dictionary lookup NULL "
                              "during separator test");
                return false;
            }
            expected  = w0;
            expected += " ";
            expected += w25;

            if (decoded != expected) {
                ESP_LOGE(TAG,
                    "Self-test JSC FAIL (separator): "
                    "expected='%s' got='%s'",
                    expected.c_str(), decoded.c_str());
                return false;
            }

            ESP_LOGI(TAG,
                "Self-test JSC: C1 sub-PASS — codeword round-trip OK at "
                "all depth boundaries (idx 0..262143) + separator test "
                "('%s')", decoded.c_str());
        }

        // ── C2: sorted-by-string lookup index ────────────────────────
        //
        // main.c already calls nanojs8_jsc_init() at boot, but we call
        // JSC::init() again here so the self-test is self-sufficient
        // (idempotent — returns true immediately if already done).
        // Verifies the index is actually built before we test lookups.
        if (!JSC::init()) {
            ESP_LOGE(TAG, "Self-test JSC: JSC::init() returned false — "
                          "cannot test sorted-index lookup paths");
            return false;
        }

        // exists() at known dictionary positions from the boot-time
        // jsc_map sanity check. If any of these mismatch, either the
        // sort is broken, the binary search is broken, or jsc_map's
        // dictionary content has shifted (in which case this test's
        // expected values need updating).
        {
            struct ExistsCase {
                const char *word;
                uint32_t    expected_idx;
            };
            static const ExistsCase exact_cases[] = {
                { "E",      0u      },
                { "+",      25u     },
                { "ROSIDS", 262143u },
            };
            for (const auto &c : exact_cases) {
                uint32_t idx = 0xDEADBEEFu;
                if (!JSC::exists(std::string(c.word), &idx)) {
                    ESP_LOGE(TAG,
                        "Self-test JSC: exists('%s') returned false "
                        "(expected idx=%u)",
                        c.word, (unsigned)c.expected_idx);
                    return false;
                }
                if (idx != c.expected_idx) {
                    ESP_LOGE(TAG,
                        "Self-test JSC: exists('%s') idx=%u "
                        "(expected %u)",
                        c.word, (unsigned)idx,
                        (unsigned)c.expected_idx);
                    return false;
                }
            }
        }

        // exists() must return false for a guaranteed-not-in-dict
        // string. If this returns true we have a false-positive bug
        // (e.g. binary search returns the closest match instead of
        // exact). The string is intentionally long and nonsense so it
        // can't accidentally match any natural-language entry.
        {
            uint32_t idx = 0xDEADBEEFu;
            if (JSC::exists(std::string("QQQQQQQQQQ12345"), &idx)) {
                ESP_LOGE(TAG,
                    "Self-test JSC: exists('QQQQQQQQQQ12345') returned "
                    "TRUE — false-positive bug (got idx=%u)",
                    (unsigned)idx);
                return false;
            }
        }

        // Round-trip: take a dictionary word at index N, look it up,
        // verify we get N back. This crosses BOTH directions of the
        // index (dict→word→sorted-index→original-dict) and catches
        // any sort-permutation or pointer-resolution bug.
        {
            static const uint32_t rt_indices[] = {
                0u, 1u, 25u, 100u, 1000u, 50000u, 100000u, 262143u
            };
            for (uint32_t test_idx : rt_indices) {
                const char *w = nanojs8_jsc_map_word(test_idx);
                if (w == nullptr || w[0] == '\0') {
                    ESP_LOGE(TAG,
                        "Self-test JSC: dict entry %u is null/empty "
                        "(jsc_map issue?)",
                        (unsigned)test_idx);
                    return false;
                }
                uint32_t found_idx = 0xDEADBEEFu;
                if (!JSC::exists(std::string(w), &found_idx)) {
                    ESP_LOGE(TAG,
                        "Self-test JSC: round-trip exists('%s') "
                        "returned false (expected idx=%u)",
                        w, (unsigned)test_idx);
                    return false;
                }
                if (found_idx != test_idx) {
                    ESP_LOGE(TAG,
                        "Self-test JSC: round-trip mismatch: word='%s' "
                        "expected idx=%u got %u",
                        w, (unsigned)test_idx, (unsigned)found_idx);
                    return false;
                }
            }
        }

        // Longest-prefix-match: lookup() char* overload. The dict has
        // single chars and short tokens at low indices, so SOME prefix
        // of "HELLO WORLD" must exist (at minimum "H" or " "). The
        // crucial check is that whatever index it returns, the word
        // at that index IS a prefix of the input — that's the safety
        // invariant compress() in C3 will rely on.
        {
            bool ok = false;
            const uint32_t idx = JSC::lookup("HELLO WORLD", &ok);
            if (!ok) {
                ESP_LOGE(TAG,
                    "Self-test JSC: lookup_prefix('HELLO WORLD') "
                    "found no prefix — even single chars should match");
                return false;
            }
            const char *matched = nanojs8_jsc_map_word(idx);
            if (matched == nullptr) {
                ESP_LOGE(TAG,
                    "Self-test JSC: lookup_prefix returned idx=%u "
                    "but nanojs8_jsc_map_word() gave NULL",
                    (unsigned)idx);
                return false;
            }
            const size_t mlen = std::strlen(matched);
            if (std::strncmp(matched, "HELLO WORLD", mlen) != 0) {
                ESP_LOGE(TAG,
                    "Self-test JSC: lookup_prefix returned non-prefix "
                    "'%s' for 'HELLO WORLD' (idx=%u)",
                    matched, (unsigned)idx);
                return false;
            }
            ESP_LOGI(TAG,
                "Self-test JSC: C2 lookup_prefix('HELLO WORLD') → "
                "idx=%u word='%s' (%u chars)",
                (unsigned)idx, matched, (unsigned)mlen);
        }

        // ── C3: compress() round-trip tests ──────────────────────────
        //
        // The point of Layer C3 is to verify that JSC::compress() produces
        // a bit stream that JSC::decompress() (which is wire-compatible
        // with stock JS8Call) reconstructs back to the original input.
        // Anything that fails this test would also fail on-air against a
        // stock JS8Call station, so this is the boot-time guardrail
        // protecting wire compatibility.
        //
        // Each test:
        //   1. Run compress() on a sample input
        //   2. Concatenate all .first BitVectors into one stream
        //   3. Sum all .second char counts → total_chars consumed
        //   4. decompress() the concatenated stream
        //   5. Verify decompressed == input.substr(0, total_chars)
        //
        // We don't require ALL chars to be consumed — chars not in the
        // dict will cause compress() to stop early. We only require that
        // whatever WAS consumed reconstructs faithfully.
        {
            struct RoundTrip {
                const char *name;
                const char *input;
                bool        require_full_consume;
            };
            static const RoundTrip cases[] = {
                { "empty",         "",                                 true  },
                { "single E",      "E",                                true  },
                { "two words",     "HELLO WORLD",                      true  },
                { "long sentence", "THE QUICK BROWN FOX",              true  },
                { "digits",        "73",                               true  },
                { "fix6-style",    "TEST , # 2 WORLD GI",              false },
            };

            for (const auto &c : cases) {
                const std::string in(c.input);
                std::vector<CodewordPair> pairs = JSC::compress(in);

                Codeword all_bits;
                uint32_t total_chars = 0;
                for (const auto &p : pairs) {
                    all_bits.insert(all_bits.end(),
                                    p.first.begin(), p.first.end());
                    total_chars += p.second;
                }

                if (total_chars > in.size()) {
                    ESP_LOGE(TAG,
                        "Self-test JSC C3 [%s]: compress consumed %u "
                        "chars but input is only %u — bounds violation",
                        c.name, (unsigned)total_chars,
                        (unsigned)in.size());
                    return false;
                }

                if (c.require_full_consume &&
                    total_chars != in.size())
                {
                    ESP_LOGE(TAG,
                        "Self-test JSC C3 [%s]: compress consumed %u of "
                        "%u chars — expected full consume of '%s'",
                        c.name, (unsigned)total_chars,
                        (unsigned)in.size(), c.input);
                    return false;
                }

                const std::string decoded = JSC::decompress(all_bits);
                const std::string expected = in.substr(0, total_chars);
                if (decoded != expected) {
                    ESP_LOGE(TAG,
                        "Self-test JSC C3 [%s]: round-trip mismatch — "
                        "expected '%s' got '%s' (consumed %u chars from "
                        "input '%s', %u pairs)",
                        c.name, expected.c_str(), decoded.c_str(),
                        (unsigned)total_chars, c.input,
                        (unsigned)pairs.size());
                    return false;
                }
            }
        }

        // ── C3 diagnostic dump ───────────────────────────────────────
        //
        // Per Dan's request: log compress() output for one representative
        // input every boot. If a future on-air test against stock JS8Call
        // fails, this dump tells us at a glance whether the issue is in
        // compress (look at per-pair idx + matched word) or downstream
        // (packCompressedMessage padding, modulator, audio).
        //
        // Sample input is chosen to exercise the separator path (3 words,
        // 2 inter-word spaces) without being so long that the log becomes
        // unwieldy.
        {
            const std::string diag_in("HELLO WORLD FROM TEST");
            std::vector<CodewordPair> pairs = JSC::compress(diag_in);

            Codeword all_bits;
            uint32_t total_chars = 0;
            for (const auto &p : pairs) {
                all_bits.insert(all_bits.end(),
                                p.first.begin(), p.first.end());
                total_chars += p.second;
            }
            const std::string decoded = JSC::decompress(all_bits);

            ESP_LOGI(TAG,
                "Self-test JSC C3 diagnostic: input='%s' (%u chars) → "
                "%u pairs, %u bits total, %u chars consumed, "
                "decompress='%s'",
                diag_in.c_str(), (unsigned)diag_in.size(),
                (unsigned)pairs.size(), (unsigned)all_bits.size(),
                (unsigned)total_chars, decoded.c_str());

            // Per-pair breakdown (cap at 10 entries to bound log volume,
            // though a 21-char input will yield at most ~6 pairs).
            const size_t to_show =
                pairs.size() < 10 ? pairs.size() : 10;
            for (size_t i = 0; i < to_show; ++i) {
                const auto &p = pairs[i];
                // To get the matched index back we'd need to re-decode
                // each pair's bits, which is overkill for a diag log.
                // Instead, decompress this pair's bits standalone — the
                // result is the word that was matched (plus trailing
                // space if separator was set).
                Codeword single = p.first;
                const std::string word = JSC::decompress(single);
                ESP_LOGI(TAG,
                    "Self-test JSC C3 diagnostic: pair[%u]: %u bits, "
                    "%u chars consumed, decodes to '%s'",
                    (unsigned)i, (unsigned)p.first.size(),
                    (unsigned)p.second, word.c_str());
            }
        }

        ESP_LOGI(TAG,
            "Self-test JSC: PASS — codeword (C1) + sorted-index "
            "lookup (C2) + compress round-trip (C3) all OK");
    }

    // ── Subtest B: full encode produces valid Costas + tone range ───────
    {
        uint8_t tones[NANOJS8_JS8_NUM_TONES];
        if (!nanojs8_js8_encode_heartbeat(test_text, test_call, tones)) {
            ESP_LOGE(TAG, "Self-test B: encode_heartbeat returned false");
            return false;
        }

        // All tones in [0,7]. (encode_heartbeat already checks this and
        // returns false otherwise; the loop here is belt-and-suspenders.)
        for (int i = 0; i < NANOJS8_JS8_NUM_TONES; ++i) {
            if (tones[i] > 7) {
                ESP_LOGE(TAG, "Self-test B: tone[%d]=%u > 7", i, tones[i]);
                return false;
            }
        }

        // Costas arrays at positions 0–6, 36–42, 72–78 must equal
        // {4,2,5,6,1,3,0}. This is the same sync pattern the RX side
        // looks for in ftx_find_candidates, so the cross-check verifies
        // TX framing matches RX expectations.
        const int offsets[3] = { kCostasOffsetStart,
                                  kCostasOffsetMiddle,
                                  kCostasOffsetEnd };
        for (int b = 0; b < 3; ++b) {
            const int o = offsets[b];
            for (int i = 0; i < 7; ++i) {
                if (tones[o + i] != kCostasJS8Normal[i]) {
                    ESP_LOGE(TAG, "Self-test B: Costas mismatch at tones[%d]: "
                                  "got %u, expected %u",
                             o + i, tones[o + i], kCostasJS8Normal[i]);
                    return false;
                }
            }
        }
        ESP_LOGI(TAG, "Self-test B: PASS — 79 tones, all in [0,7], "
                      "Costas correct at positions 0-6, 36-42, 72-78");

        // Log first 12 tones at DEBUG level so the audit trail shows the
        // actual numeric output for any later regression chase.
        ESP_LOGI(TAG, "Self-test B: tones[0..11] = "
                      "%u %u %u %u %u %u %u  %u %u %u %u %u",
                 tones[0], tones[1], tones[2], tones[3],
                 tones[4], tones[5], tones[6],
                 tones[7], tones[8], tones[9],
                 tones[10], tones[11]);
    }

    // ── Subtest C (L7.11b): modulator alloc + sample generation ──────────
    // Implemented in js8_modulate.c (pure C) — orchestrated here so the
    // operator sees one PASS/FAIL gate for the whole TX chain at boot.
    if (!nanojs8_js8_modulate_self_test()) {
        ESP_LOGE(TAG, "Self-test C failed — TX chain not operational");
        return false;
    }

    ESP_LOGI(TAG, "Self-test: ALL PASS — TX encoder operational");
    return true;
}

} // extern "C"
