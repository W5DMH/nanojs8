// NanoJS8 — JSC dictionary stub.
//
// This file provides empty JSC tables while preserving the API surface that
// gfsk8-modem-clean's decoder expects. Used in conjunction with patch
// 0002-jsc-stub.patch which removes the original heavyweight JSC.cpp tables.
//
// JS8 2.2+ deprecates JSC compression; NanoJS8 never transmits JSC and
// surfaces incoming JSC payloads to the UI as opaque type=JSC frames.
//
// This file is only compiled into the final binary in Phase 4 onward, when
// gfsk8_modem is added to main/CMakeLists.txt REQUIRES.

// We deliberately don't include the upstream JSC header here in Phase 0 —
// the file is parsed by CMake's file(GLOB) but won't be compiled until
// upstream/ exists. This avoids a missing-include compiler error during
// Phase 0 builds.

#if __has_include("JSC.h")
#include "JSC.h"

namespace JSC {

// Empty map[] — JSC compression dictionary lookup, deliberately deactivated.
// The decoder must check for an empty map and fall through to Huffman.
const std::vector<std::pair<std::string, std::uint16_t>> map = {};

// Empty list[] — reverse lookup. Deactivated symmetrically.
const std::vector<std::string> list = {};

// prefix[] is NOT redefined here — upstream's definition remains intact.
// The decoder uses prefix[] for extended-character handling regardless of
// whether JSC compression is active.

} // namespace JSC

#endif // __has_include("JSC.h")
