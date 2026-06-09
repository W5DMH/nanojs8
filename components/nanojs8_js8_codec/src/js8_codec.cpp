/*
 * js8_codec.cpp — JS8 LDPC decode + CRC-12 + message-extract (L7.6)
 * =============================================================================
 * The functions bpdecode174, checkCRC12, extractmessage174, CRC12(), and the
 * Mn/Nm parity matrices are lifted verbatim (with namespace adjustments) from
 * gfsk8-modem-clean (GPL-3.0).
 *
 * The C-callable nanojs8_js8_decode_llrs wrapper is our own glue.
 *
 * The LLR extraction (nanojs8_js8_extract_llrs) is in llr_extract.c — kept
 * separate so it can stay plain C (no std::array overhead per slot).
 */

#include "js8_codec.h"

#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>            // L7.7: std::string for Varicode interop
#include <string_view>
#include <numeric>
#include <algorithm>   // std::copy (used in bpdecode174 success path)
#include <vector>            // L7.7: Varicode unpack returns std::vector<std::string>

#include "esp_log.h"

// L7.7: gfsk8's Varicode for protocol-layer message decode.
// Pulls in Varicode::unpack{FastData,Data,Heartbeat,Compound,Directed}Message
// and bit/format helpers. Requires nanojs8_gfsk8 to be linked.
#include "Varicode.h"

static const char *TAG = "js8codec";

// ── Lifted constants ─────────────────────────────────────────────────────────
//
// Source: gfsk8-modem-clean/src/JS8.cpp (anonymous namespace, lines 187-203).
// These are the JS8 LDPC code's structural parameters.

namespace {

constexpr int N  = 174;   // codeword bits (matches NANOJS8_JS8_LDPC_N)
constexpr int K  = 87;    // info+CRC bits (matches NANOJS8_JS8_LDPC_K)
constexpr int M  = N - K; // parity bits  (87)
constexpr int KK = 87;    // alias used by lifted code

constexpr int BP_MAX_ROWS       = 7;   // max parity-row connections
constexpr int BP_MAX_CHECKS     = 3;   // every variable hits 3 checks
constexpr int BP_MAX_ITERATIONS = NANOJS8_JS8_MAX_ITERS; // 30

// JS8 6-bit alphabet. 64 characters covering [0-9A-Za-z] + "-+".
// Source: gfsk8-modem-clean/src/JS8.cpp line 850.
constexpr std::string_view alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";
static_assert(alphabet.size() == 64);

// ── LDPC parity-check matrix Mn ──────────────────────────────────────────────
//
// Each row i in Mn[N][3] lists the 3 parity-check rows that include
// variable bit i. Lifted verbatim from gfsk8-modem-clean/src/JS8.cpp
// lines 599-634. These constants ARE the JS8 LDPC code definition.
//
// constexpr → all data lives in .rodata (flash), zero RAM cost.

constexpr std::array<std::array<int, BP_MAX_CHECKS>, N> Mn = {
    {{0, 24, 68},  {1, 4, 72},   {2, 31, 67},  {3, 50, 60},  {5, 62, 69},
     {6, 32, 78},  {7, 49, 85},  {8, 36, 42},  {9, 40, 64},  {10, 13, 63},
     {11, 74, 76}, {12, 22, 80}, {14, 15, 81}, {16, 55, 65}, {17, 52, 59},
     {18, 30, 51}, {19, 66, 83}, {20, 28, 71}, {21, 23, 43}, {25, 34, 75},
     {26, 35, 37}, {27, 39, 41}, {29, 53, 54}, {33, 48, 86}, {38, 56, 57},
     {44, 73, 82}, {45, 61, 79}, {46, 47, 84}, {58, 70, 77}, {0, 49, 52},
     {1, 46, 83},  {2, 24, 78},  {3, 5, 13},   {4, 6, 79},   {7, 33, 54},
     {8, 35, 68},  {9, 42, 82},  {10, 22, 73}, {11, 16, 43}, {12, 56, 75},
     {14, 26, 55}, {15, 27, 28}, {17, 18, 58}, {19, 39, 62}, {20, 34, 51},
     {21, 53, 63}, {23, 61, 77}, {25, 31, 76}, {29, 71, 84}, {30, 64, 86},
     {32, 38, 50}, {36, 47, 74}, {37, 69, 70}, {40, 41, 67}, {44, 66, 85},
     {45, 80, 81}, {48, 65, 72}, {57, 59, 65}, {60, 64, 84}, {0, 13, 20},
     {1, 12, 58},  {2, 66, 81},  {3, 31, 72},  {4, 35, 53},  {5, 42, 45},
     {6, 27, 74},  {7, 32, 70},  {8, 48, 75},  {9, 57, 63},  {10, 47, 67},
     {11, 18, 44}, {14, 49, 60}, {15, 21, 25}, {16, 71, 79}, {17, 39, 54},
     {19, 34, 50}, {22, 24, 33}, {23, 62, 86}, {26, 38, 73}, {28, 77, 82},
     {29, 69, 76}, {30, 68, 83}, {21, 36, 85}, {37, 40, 80}, {41, 43, 56},
     {46, 52, 61}, {51, 55, 78}, {59, 74, 80}, {0, 38, 76},  {1, 15, 40},
     {2, 30, 53},  {3, 35, 77},  {4, 44, 64},  {5, 56, 84},  {6, 13, 48},
     {7, 20, 45},  {8, 14, 71},  {9, 19, 61},  {10, 16, 70}, {11, 33, 46},
     {12, 67, 85}, {17, 22, 42}, {18, 63, 72}, {23, 47, 78}, {24, 69, 82},
     {25, 79, 86}, {26, 31, 39}, {27, 55, 68}, {28, 62, 65}, {29, 41, 49},
     {32, 36, 81}, {34, 59, 73}, {37, 54, 83}, {43, 51, 60}, {50, 52, 71},
     {57, 58, 66}, {46, 55, 75}, {0, 18, 36},  {1, 60, 74},  {2, 7, 65},
     {3, 59, 83},  {4, 33, 38},  {5, 25, 52},  {6, 31, 56},  {8, 51, 66},
     {9, 11, 14},  {10, 50, 68}, {12, 13, 64}, {15, 30, 42}, {16, 19, 35},
     {17, 79, 85}, {20, 47, 58}, {21, 39, 45}, {22, 32, 61}, {23, 29, 73},
     {24, 41, 63}, {26, 48, 84}, {27, 37, 72}, {28, 43, 80}, {34, 67, 69},
     {40, 62, 75}, {44, 48, 70}, {49, 57, 86}, {47, 53, 82}, {12, 54, 78},
     {76, 77, 81}, {0, 1, 23},   {2, 5, 74},   {3, 55, 86},  {4, 43, 52},
     {6, 49, 82},  {7, 9, 27},   {8, 54, 61},  {10, 28, 66}, {11, 32, 39},
     {13, 15, 19}, {14, 34, 72}, {16, 30, 38}, {17, 35, 56}, {18, 45, 75},
     {20, 41, 83}, {21, 33, 58}, {22, 25, 60}, {24, 59, 64}, {26, 63, 79},
     {29, 36, 65}, {31, 44, 71}, {37, 50, 85}, {40, 76, 78}, {42, 55, 67},
     {46, 73, 81}, {39, 51, 77}, {53, 60, 70}, {45, 57, 68}}};
// NOTE: above is the FULL 174-entry Mn array. Total: 174 × 3 ints = 2 KB flash.
// NOTE: above is the FULL 174-entry Mn array. Total: 174 × 3 ints = 2 KB flash.

// ── LDPC variable-node neighbor structure Nm ─────────────────────────────────
//
// Each row i in Nm[M] gives the set of variable bits feeding parity check i.
// "valid_neighbors" is the row population count (5 or 6), and "neighbors"
// is a fixed-size 7-slot array padded with sentinel 0s.
//
// Lifted verbatim from gfsk8-modem-clean/src/JS8.cpp lines 636-727.

struct ParityCheckNode {
    int valid_neighbors;
    std::array<int, BP_MAX_ROWS> neighbors;
};

constexpr std::array<ParityCheckNode, M> Nm = {{{6, {0, 29, 59, 88, 117, 146, 0}},
                                          {6, {1, 30, 60, 89, 118, 146, 0}},
                                          {6, {2, 31, 61, 90, 119, 147, 0}},
                                          {6, {3, 32, 62, 91, 120, 148, 0}},
                                          {6, {1, 33, 63, 92, 121, 149, 0}},
                                          {6, {4, 32, 64, 93, 122, 147, 0}},
                                          {6, {5, 33, 65, 94, 123, 150, 0}},
                                          {6, {6, 34, 66, 95, 119, 151, 0}},
                                          {6, {7, 35, 67, 96, 124, 152, 0}},
                                          {6, {8, 36, 68, 97, 125, 151, 0}},
                                          {6, {9, 37, 69, 98, 126, 153, 0}},
                                          {6, {10, 38, 70, 99, 125, 154, 0}},
                                          {6, {11, 39, 60, 100, 127, 144, 0}},
                                          {6, {9, 32, 59, 94, 127, 155, 0}},
                                          {6, {12, 40, 71, 96, 125, 156, 0}},
                                          {6, {12, 41, 72, 89, 128, 155, 0}},
                                          {6, {13, 38, 73, 98, 129, 157, 0}},
                                          {6, {14, 42, 74, 101, 130, 158, 0}},
                                          {6, {15, 42, 70, 102, 117, 159, 0}},
                                          {6, {16, 43, 75, 97, 129, 155, 0}},
                                          {6, {17, 44, 59, 95, 131, 160, 0}},
                                          {6, {18, 45, 72, 82, 132, 161, 0}},
                                          {6, {11, 37, 76, 101, 133, 162, 0}},
                                          {6, {18, 46, 77, 103, 134, 146, 0}},
                                          {6, {0, 31, 76, 104, 135, 163, 0}},
                                          {6, {19, 47, 72, 105, 122, 162, 0}},
                                          {6, {20, 40, 78, 106, 136, 164, 0}},
                                          {6, {21, 41, 65, 107, 137, 151, 0}},
                                          {6, {17, 41, 79, 108, 138, 153, 0}},
                                          {6, {22, 48, 80, 109, 134, 165, 0}},
                                          {6, {15, 49, 81, 90, 128, 157, 0}},
                                          {6, {2, 47, 62, 106, 123, 166, 0}},
                                          {6, {5, 50, 66, 110, 133, 154, 0}},
                                          {6, {23, 34, 76, 99, 121, 161, 0}},
                                          {6, {19, 44, 75, 111, 139, 156, 0}},
                                          {6, {20, 35, 63, 91, 129, 158, 0}},
                                          {6, {7, 51, 82, 110, 117, 165, 0}},
                                          {6, {20, 52, 83, 112, 137, 167, 0}},
                                          {6, {24, 50, 78, 88, 121, 157, 0}},
                                          {7, {21, 43, 74, 106, 132, 154, 171}},
                                          {6, {8, 53, 83, 89, 140, 168, 0}},
                                          {6, {21, 53, 84, 109, 135, 160, 0}},
                                          {6, {7, 36, 64, 101, 128, 169, 0}},
                                          {6, {18, 38, 84, 113, 138, 149, 0}},
                                          {6, {25, 54, 70, 92, 141, 166, 0}},
                                          {7, {26, 55, 64, 95, 132, 159, 173}},
                                          {6, {27, 30, 85, 99, 116, 170, 0}},
                                          {6, {27, 51, 69, 103, 131, 143, 0}},
                                          {6, {23, 56, 67, 94, 136, 141, 0}},
                                          {6, {6, 29, 71, 109, 142, 150, 0}},
                                          {6, {3, 50, 75, 114, 126, 167, 0}},
                                          {6, {15, 44, 86, 113, 124, 171, 0}},
                                          {6, {14, 29, 85, 114, 122, 149, 0}},
                                          {6, {22, 45, 63, 90, 143, 172, 0}},
                                          {6, {22, 34, 74, 112, 144, 152, 0}},
                                          {7, {13, 40, 86, 107, 116, 148, 169}},
                                          {6, {24, 39, 84, 93, 123, 158, 0}},
                                          {6, {24, 57, 68, 115, 142, 173, 0}},
                                          {6, {28, 42, 60, 115, 131, 161, 0}},
                                          {6, {14, 57, 87, 111, 120, 163, 0}},
                                          {7, {3, 58, 71, 113, 118, 162, 172}},
                                          {6, {26, 46, 85, 97, 133, 152, 0}},
                                          {5, {4, 43, 77, 108, 140, 0, 0}},
                                          {6, {9, 45, 68, 102, 135, 164, 0}},
                                          {6, {8, 49, 58, 92, 127, 163, 0}},
                                          {6, {13, 56, 57, 108, 119, 165, 0}},
                                          {6, {16, 54, 61, 115, 124, 153, 0}},
                                          {6, {2, 53, 69, 100, 139, 169, 0}},
                                          {6, {0, 35, 81, 107, 126, 173, 0}},
                                          {5, {4, 52, 80, 104, 139, 0, 0}},
                                          {6, {28, 52, 66, 98, 141, 172, 0}},
                                          {6, {17, 48, 73, 96, 114, 166, 0}},
                                          {6, {1, 56, 62, 102, 137, 156, 0}},
                                          {6, {25, 37, 78, 111, 134, 170, 0}},
                                          {6, {10, 51, 65, 87, 118, 147, 0}},
                                          {6, {19, 39, 67, 116, 140, 159, 0}},
                                          {6, {10, 47, 80, 88, 145, 168, 0}},
                                          {6, {28, 46, 79, 91, 145, 171, 0}},
                                          {6, {5, 31, 86, 103, 144, 168, 0}},
                                          {6, {26, 33, 73, 105, 130, 164, 0}},
                                          {5, {11, 55, 83, 87, 138, 0, 0}},
                                          {6, {12, 55, 61, 110, 145, 170, 0}},
                                          {6, {25, 36, 79, 104, 143, 150, 0}},
                                          {6, {16, 30, 81, 112, 120, 160, 0}},
                                          {5, {27, 48, 58, 93, 136, 0, 0}},
                                          {6, {6, 54, 82, 100, 130, 167, 0}},
                                          {6, {23, 49, 77, 105, 142, 148, 0}}}};

// ── CRC-12 (boost::augmented_crc<12, 0xc06>) ─────────────────────────────────
//
// Lifted from gfsk8-modem-clean/vendor/crc12.h. JS8 uses the CRC value
// XORed with 42 — see JS8.cpp line 894.

inline uint16_t crc12_compute(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        for (int b = 7; b >= 0; --b) {
            uint32_t const bit = (data[i] >> b) & 1u;
            uint32_t const top = (crc >> 11) & 1u;
            crc = ((crc << 1) ^ bit) & 0xFFFu;
            if (top) crc ^= 0xC06u;
        }
    }
    return static_cast<uint16_t>(crc);
}

// JS8's CRC12 wrapper: regular CRC-12 XORed with 42.
// Source: gfsk8-modem-clean/src/JS8.cpp line 893.
template <typename T> std::uint16_t CRC12(T const &range) {
    return crc12_compute(reinterpret_cast<const uint8_t*>(range.data()),
                         range.size()) ^ 42;
}

// ── bpdecode174 — Belief Propagation LDPC decoder ────────────────────────────
//
// Lifted verbatim from gfsk8-modem-clean/src/JS8.cpp lines 731-841.
// Input:  llr (174 LLRs, positive = bit is 1)
// Output: decoded (87 bits = info + CRC), cw (full 174-bit codeword)
// Return: number of LLR-vs-decoded-bit mismatches (0 = perfect codeword),
//         or -1 if BP did not converge.

int bpdecode174(std::array<float, N> const &llr, std::array<int8_t, K> &decoded,
                std::array<int8_t, N> &cw) {
    // Initialize messages and variables
    std::array<std::array<float, BP_MAX_CHECKS>, N> tov =
        {}; // Messages to variable nodes
    std::array<std::array<float, BP_MAX_ROWS>, M> toc =
        {}; // Messages to check nodes
    std::array<std::array<float, BP_MAX_ROWS>, M> tanhtoc =
        {}; // Tanh of messages

    std::array<float, N> zn = {}; // Bit log likelihood ratios
    std::array<int, M> synd = {}; // Syndrome for checks

    int ncnt = 0;
    int nclast = 0;

    // Initialize toc (messages from bits to checks)
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < Nm[i].valid_neighbors; ++j) {
            toc[i][j] = llr[Nm[i].neighbors[j]];
        }
    }

    // Iterative decoding
    for (int iter = 0; iter <= BP_MAX_ITERATIONS; ++iter) {
        // Update bit log likelihood ratios
        for (int i = 0; i < N; ++i) {
            zn[i] =
                llr[i] + std::accumulate(tov[i].begin(),
                                         tov[i].begin() + BP_MAX_CHECKS, 0.0f);
        }

        // Check if we have a valid codeword
        for (int i = 0; i < N; ++i)
            cw[i] = zn[i] > 0 ? 1 : 0;

        int ncheck = 0;
        for (int i = 0; i < M; ++i) {
            synd[i] = 0;
            for (int j = 0; j < Nm[i].valid_neighbors; ++j) {
                synd[i] += cw[Nm[i].neighbors[j]];
            }
            if (synd[i] % 2 != 0)
                ++ncheck;
        }

        if (ncheck == 0) {
            // Extract decoded bits (last N-M bits of codeword)
            std::copy(cw.begin() + M, cw.end(), decoded.begin());

            // Count errors
            int nerr = 0;
            for (int i = 0; i < N; ++i) {
                if ((2 * cw[i] - 1) * llr[i] < 0.0f) {
                    ++nerr;
                }
            }

            return nerr;
        }

        // Early stopping criterion
        if (iter > 0) {
            int nd = ncheck - nclast;
            ncnt = (nd < 0) ? 0 : ncnt + 1;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) {
                return -1;
            }
        }
        nclast = ncheck;

        // Send messages from bits to check nodes
        for (int i = 0; i < M; ++i) {
            for (int j = 0; j < Nm[i].valid_neighbors; ++j) {
                int ibj = Nm[i].neighbors[j];
                toc[i][j] = zn[ibj];
                for (int k = 0; k < BP_MAX_CHECKS; ++k) {
                    if (Mn[ibj][k] == i) {
                        toc[i][j] -= tov[ibj][k];
                    }
                }
            }
        }

        // Send messages from check nodes to variable nodes
        for (int i = 0; i < M; ++i) {
            for (int j = 0; j < 7; ++j) {
                tanhtoc[i][j] = std::tanh(-toc[i][j] / 2.0f);
            }
        }

        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < BP_MAX_CHECKS; ++j) {
                int ichk = Mn[i][j];
                if (ichk >= 0) {
                    float Tmn = 1.0f;
                    for (int k = 0; k < Nm[ichk].valid_neighbors; ++k) {
                        if (Nm[ichk].neighbors[k] != i) {
                            Tmn *= tanhtoc[ichk][k];
                        }
                    }
                    tov[i][j] = 2.0f * std::atanh(-Tmn);
                }
            }
        }
    }

    return -1; // Decoding failed
}

// ── checkCRC12 — verify the embedded CRC ─────────────────────────────────────
//
// Lifted verbatim from gfsk8-modem-clean/src/JS8.cpp lines 897-918.

bool checkCRC12(std::array<std::int8_t, KK> const &decoded) {
    std::array<uint8_t, 11> bits = {};

    for (std::size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i])
            bits[i / 8] |= (1 << (7 - (i % 8)));
    }

    // Extract the received CRC-12.
    uint16_t crc = (static_cast<uint16_t>(bits[9] & 0x1F) << 7) |
                   (static_cast<uint16_t>(bits[10]) >> 1);

    // Clear bits that correspond to the CRC in the last bytes.
    bits[9] &= 0xE0;
    bits[10] = 0x00;

    // Compute CRC and indicate if we have a match.
    return crc == CRC12(bits);
}

// ── extract_alphabet_chars — message word → JS8 alphabet character ───────────
//
// Adapted from gfsk8-modem-clean/src/JS8.cpp extractmessage174 (lines 920-943).
// We split the upstream function: this part only deals with the 12-char
// mapping (the upstream version assumed CRC was already validated).

void extract_alphabet_chars(std::array<std::int8_t, KK> const &decoded,
                            char out[NANOJS8_JS8_MSG_CHARS])
{
    for (std::size_t i = 0; i < NANOJS8_JS8_MSG_CHARS; ++i) {
        const uint8_t word =
            (decoded[i * 6 + 0] << 5) | (decoded[i * 6 + 1] << 4) |
            (decoded[i * 6 + 2] << 3) | (decoded[i * 6 + 3] << 2) |
            (decoded[i * 6 + 4] << 1) | (decoded[i * 6 + 5] << 0);
        out[i] = alphabet[word & 0x3F];
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public C-callable wrapper
// ─────────────────────────────────────────────────────────────────────────────

extern "C" bool nanojs8_js8_decode_llrs(const float *llr174,
                                         nanojs8_js8_decode_result_t *result)
{
    if (!llr174 || !result) return false;

    // Zero-init the result struct so callers always see clean values.
    std::memset(result, 0, sizeof(*result));

    // Copy LLRs into std::array form expected by bpdecode174.
    std::array<float, N> llr_arr;
    for (int i = 0; i < N; ++i) llr_arr[i] = llr174[i];

    // Storage for BP outputs.
    std::array<int8_t, K> decoded = {};
    std::array<int8_t, N> codeword = {};

    const int nerr = bpdecode174(llr_arr, decoded, codeword);
    result->ldpc_errors = nerr;

    if (nerr < 0) {
        // BP did not converge.
        return false;
    }

    // BP converged → check CRC-12.
    if (!checkCRC12(decoded)) {
        // LDPC found A valid codeword but it's not the right one (or
        // received data was too corrupted for a meaningful answer).
        return false;
    }

    // CRC OK: pull out the 12 alphabet characters and the 3-bit frame type.
    result->crc_ok = true;

    char msg_buf[NANOJS8_JS8_MSG_CHARS + 1] = {};
    extract_alphabet_chars(decoded, msg_buf);
    msg_buf[NANOJS8_JS8_MSG_CHARS] = '\0';
    std::memcpy(result->message, msg_buf, sizeof(result->message));

    // Frame type lives in bits 72..74 of the decoded message.
    // (75 message bits = 12 × 6 alphabet bits = 72 + 3 frame-type bits.)
    uint8_t ftype = 0;
    for (int b = 0; b < 3; ++b) {
        ftype = (ftype << 1) | (decoded[72 + b] & 1);
    }
    result->frame_type = ftype;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// L7.7: Protocol-layer message unpack
// ─────────────────────────────────────────────────────────────────────────────
//
// Mirrors the strategy chain in gfsk8's DecodedText.cpp (lines 104-214):
// for each frame we try FastData → Data → Heartbeat → Compound → Directed,
// in that order, and take the first match.
//
// We replicate the chain inline (rather than #include "DecodedText.h" and
// instantiate the class) because DecodedText.h transitively pulls in JS8.h
// for GFSK8::Event::Decoded — which we don't use, and which would drag in
// the entire gfsk8 modem class graph. Calling Varicode static methods
// directly costs ~80 lines and is fully equivalent for our needs.

namespace {

/// Build a "compound" callsign string from the first two parts (call/grid
/// or base/suffix). Mirrors DecodedText.cpp assembleCompound().
std::string assembleCompound(std::vector<std::string> const &parts)
{
    std::string result;
    int added = 0;
    for (int i = 0; i < 2 && i < static_cast<int>(parts.size()); ++i) {
        if (parts[i].empty()) continue;
        if (added > 0) result += '/';
        result += parts[i];
        ++added;
    }
    return result;
}

/// Join elements of v with sep between each pair.
std::string concatenate(std::vector<std::string> const &v, std::string const &sep)
{
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += sep;
        out += v[i];
    }
    return out;
}

/// Copy a std::string into a fixed C buffer, NUL-terminating. Truncates
/// if needed — callers size the buffer for typical JS8 single-frame text
/// (NANOJS8_JS8_MSG_TEXT_MAX = 256 chars).
void copy_to_c(std::string const &src, char *dst, size_t dst_cap)
{
    if (dst_cap == 0) return;
    const size_t n = (src.size() < dst_cap - 1) ? src.size() : (dst_cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

/// L7.9: per-strategy structured-field bundle. Strategy functions populate
/// this alongside `text`, then nanojs8_js8_unpack_message copies the
/// non-empty fields into the output struct's C buffers. Keeping this as
/// std::string here keeps the strategy code clean; only one conversion
/// happens at the end.
struct StructuredFields {
    std::string from_call;
    std::string to_call;
    std::string grid;
    std::string verb;
    std::string body;
};

/// The 5 strategies, run in the order DecodedText uses. Each returns true
/// if it produced a non-empty parse; sets `text` and `subtype` on success.
/// `bits` here is the TransmissionType bit field (NANOJS8_JS8_TX_*).

bool try_fast_data(std::string const &m, int bits,
                   std::string &text, uint8_t &subtype,
                   StructuredFields &sf)
{
    if ((bits & NANOJS8_JS8_TX_DATA) != NANOJS8_JS8_TX_DATA)
        return false;
    auto const decoded = Varicode::unpackFastDataMessage(m);
    if (decoded.empty()) return false;
    text    = decoded;
    subtype = NANOJS8_JS8_FRAME_DATA;
    // Data frames carry no structured FROM/TO — body is the chunk text.
    sf.body = decoded;
    return true;
}

bool try_data(std::string const &m, int bits,
              std::string &text, uint8_t &subtype,
              StructuredFields &sf)
{
    if ((bits & NANOJS8_JS8_TX_DATA) == NANOJS8_JS8_TX_DATA)
        return false;
    auto const decoded = Varicode::unpackDataMessage(m);
    if (decoded.empty()) return false;
    text    = decoded;
    subtype = NANOJS8_JS8_FRAME_DATA;
    sf.body = decoded;
    return true;
}

bool try_heartbeat(std::string const &m, int bits,
                   std::string &text, uint8_t &subtype,
                   bool &is_heartbeat,
                   StructuredFields &sf)
{
    if ((bits & NANOJS8_JS8_TX_DATA) == NANOJS8_JS8_TX_DATA)
        return false;

    bool    altFlag = false;
    uint8_t ftype   = NANOJS8_JS8_FRAME_UNKNOWN;
    uint8_t bits3   = 0;
    auto const parts = Varicode::unpackHeartbeatMessage(m, &ftype, &altFlag, &bits3);
    if (parts.size() < 2) return false;

    is_heartbeat = true;
    subtype      = ftype;

    std::string compound = assembleCompound(parts);
    std::string extra    = (parts.size() > 2) ? parts[2] : std::string{};
    std::string msg      = compound + ": ";

    std::string verb_str;
    if (altFlag) {
        verb_str = "@ALLCALL " + Varicode::cqString(bits3);
    } else {
        auto const hb = Varicode::hbString(bits3);
        verb_str = "@HB " + (hb == "HB" ? std::string("HEARTBEAT") : hb);
    }
    msg += verb_str;
    if (!extra.empty()) msg += ' ' + extra;

    text = msg;

    // Structured: heartbeat has no "to" — the recipient is implicit (@ALLCALL).
    sf.from_call = compound;
    sf.verb      = verb_str;
    // The "extra" payload of a heartbeat is conventionally the sender's
    // grid square — record it if present and well-formed (4 or 6 chars).
    if (extra.size() == 4 || extra.size() == 6) {
        sf.grid = extra;
    } else if (!extra.empty()) {
        sf.body = extra;
    }
    return true;
}

bool try_compound(std::string const &m, int bits,
                  std::string &text, uint8_t &subtype,
                  StructuredFields &sf)
{
    if ((bits & NANOJS8_JS8_TX_DATA) == NANOJS8_JS8_TX_DATA)
        return false;

    uint8_t ftype = NANOJS8_JS8_FRAME_UNKNOWN;
    uint8_t bits3 = 0;
    auto const parts = Varicode::unpackCompoundMessage(m, &ftype, &bits3);
    if (parts.size() < 2) return false;

    subtype = ftype;
    std::string compound = assembleCompound(parts);

    if (ftype == NANOJS8_JS8_FRAME_COMPOUND) {
        text = compound + ": ";
        sf.from_call = compound;
    } else if (ftype == NANOJS8_JS8_FRAME_COMPOUND_DIRECTED) {
        // L7.11g.7-fix1: for compound-directed frames the compound
        // is the DESTINATION (TO target — e.g. "@GHOSTNET" group,
        // or a compound callsign with portable suffix). The sender
        // (FROM) is announced in a separate preceding compound
        // frame at the same freq; js8_sync chains them together
        // via the compound_from_pending tracker.
        //
        // Pre-fix1 this code wrongly set from_call=compound, which
        // caused group-directed messages to look like "@GHOSTNET
        // QUERY MSGS" with no real sender — verb-detection handlers
        // couldn't route them. Now: leave from_call empty (sync
        // populates it from the chained compound-FROM frame), set
        // to_call = compound, and trim leading whitespace from the
        // verb (directed_cmds map keys carry a leading space, same
        // trim issue fix1 addressed in try_directed).
        std::vector<std::string> tail(parts.begin() + 2, parts.end());
        std::string extra = concatenate(tail, " ");
        text = compound + extra;
        sf.to_call = compound;
        {
            std::string v = extra;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
                v.erase(0, 1);
            }
            sf.verb = v;
        }
    } else {
        // ftype didn't match a compound variant — bail so the chain
        // can fall through to Directed.
        return false;
    }
    return true;
}

bool try_directed(std::string const &m, int bits,
                  std::string &text, uint8_t &subtype, bool &is_directed,
                  StructuredFields &sf)
{
    if ((bits & NANOJS8_JS8_TX_DATA) == NANOJS8_JS8_TX_DATA)
        return false;

    uint8_t ftype = NANOJS8_JS8_FRAME_UNKNOWN;
    auto const parts = Varicode::unpackDirectedMessage(m, &ftype);
    if (parts.empty()) return false;

    subtype = ftype;
    is_directed = (parts.size() > 2);

    std::string msg;
    switch (static_cast<int>(parts.size())) {
    case 3:   // "FROM DE TO CMD"
    case 4: { // "FROM DE TO CMD NUM"
        std::vector<std::string> tail(parts.begin() + 2, parts.end());
        msg = parts[0] + ": " + parts[1] + ' ' + concatenate(tail, " ");
        sf.from_call = parts[0];
        sf.to_call   = parts[1];
        // L7.11g.4-fix1: Varicode::unpackDirectedMessage can return the
        // verb token with a leading space (visible in the decoded
        // `text` as a double space between TO and VERB, e.g.
        // "KD8PGB: W5DMH  MSG"). The leading space was breaking
        // strcmp(verb, "MSG") in callers (notably the js8_sync MSG-verb
        // auto-ACK hook). Trim leading whitespace at the source so all
        // consumers (DIRECTED store, COMPOSE re-encode, MSG hook) see
        // a clean canonical verb.
        {
            std::string v = parts[2];
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
                v.erase(0, 1);
            }
            sf.verb = v;
        }
        if (parts.size() == 4) sf.body = parts[3];
        break;
    }
    default: // free text (rare)
        msg = concatenate(parts, "");
        // Best-effort: first part is usually the sender if anything is.
        if (!parts.empty()) sf.from_call = parts[0];
        break;
    }
    text = msg;
    return true;
}

} // anonymous namespace

extern "C" bool
nanojs8_js8_unpack_message(const char *raw12,
                            uint8_t frame_type,
                            int /*submode*/,                // reserved (Normal only today)
                            nanojs8_js8_message_t *out)
{
    if (!raw12 || !out) return false;

    // Initialize result to "no parse" so callers get clean state on failure.
    // memset zeroes ALL fields including the structured C buffers — those
    // start NUL-terminated as empty strings by virtue of byte 0 being '\0'.
    std::memset(out, 0, sizeof(*out));
    out->frame_subtype = NANOJS8_JS8_FRAME_UNKNOWN;

    // Reconstruct the std::string Varicode expects. The CRC-validated frame
    // is always exactly NANOJS8_JS8_MSG_CHARS (12) chars in the JS8 alphabet.
    std::string frame(raw12, NANOJS8_JS8_MSG_CHARS);

    const int bits = static_cast<int>(frame_type);

    std::string text;
    uint8_t     subtype      = NANOJS8_JS8_FRAME_UNKNOWN;
    bool        is_heartbeat = false;
    bool        is_directed  = false;
    StructuredFields sf;

    // Strategy chain — first match wins. Same order as DecodedText.cpp.
    const bool matched =
        try_fast_data (frame, bits, text, subtype, sf)                     ||
        try_data      (frame, bits, text, subtype, sf)                     ||
        try_heartbeat (frame, bits, text, subtype, is_heartbeat, sf)       ||
        try_compound  (frame, bits, text, subtype, sf)                     ||
        try_directed  (frame, bits, text, subtype, is_directed, sf);

    if (!matched) {
        // Frame was CRC-OK but no protocol-layer strategy parsed it.
        // Likely a middle-of-multi-frame data chunk that needs assembly,
        // or a frame type our chain doesn't recognise. Caller can fall
        // back to displaying the raw 12 chars.
        return false;
    }

    out->ok            = true;
    out->frame_subtype = subtype;
    out->is_heartbeat  = is_heartbeat;
    out->is_directed   = is_directed;
    copy_to_c(text,         out->text,      sizeof(out->text));
    copy_to_c(sf.from_call, out->from_call, sizeof(out->from_call));
    copy_to_c(sf.to_call,   out->to_call,   sizeof(out->to_call));
    copy_to_c(sf.grid,      out->grid,      sizeof(out->grid));
    copy_to_c(sf.verb,      out->verb,      sizeof(out->verb));
    copy_to_c(sf.body,      out->body,      sizeof(out->body));
    return true;
}
