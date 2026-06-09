# nanojs8_gfsk8 — vendored gfsk8-modem-clean for JS8 RX
#
# This component wraps Jordan Sherer's (KN4CRD) gfsk8-modem-clean as an
# ESP-IDF component. Upstream source: https://github.com/jfrancis42/gfsk8-modem-clean
# Original license: GPL-3.0. NanoJS8 inherits GPL-3.0.
#
# See ATTRIBUTION.md in this directory for the full list of modifications
# we made to the vendor source.
#
# Excluded from the lift (TX-only / UI utilities):
#   - JSC_list.cpp    (262,144-entry TX lookup table, ~6 MB)
#   - JSC_map.cpp     (262,144-entry RX dictionary — replaced by memory-mapped
#                     `jsc_map` partition; see components/nanojs8_jsc_map)
#   - JSC_checker.cpp (Qt-based spell-checker for the UI)
#
# Sources retained but functions stubbed for our RX-only build:
#   - JSC.cpp: compress(), codeword(), exists(), lookup() — see ATTRIBUTION.md
#   - Varicode.cpp: extendedChars() — dead code, no callers
#
# Requirements:
#   - C++20 (gfsk8 uses std::span, <concepts>, std::numbers)
#   - Exceptions enabled (gfsk8 has ~9 throws, plus implicit STL throws —
#     CONFIG_COMPILER_CXX_EXCEPTIONS=y in sdkconfig.defaults)
#   - PSRAM BSS placement (1.44 MB dec_data global — see api.cpp,
#     CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y)

set(GFSK8_SRCS
    src/JS8.cpp
    src/JS8Submode.cpp
    src/Varicode.cpp
    src/JSC.cpp
    src/DecodedText.cpp
    src/FrequencyTracker.cpp
    src/js8codec.cpp
    src/api.cpp
    # Vendored KissFFT — ft8_lib is removed as of L7.4b so no symbol
    # collision; we use the upstream KissFFT names directly.
    vendor/kissfft/kiss_fft.c
    vendor/kissfft/kiss_fftr.c
)

idf_component_register(
    SRCS         ${GFSK8_SRCS}
    INCLUDE_DIRS "include"
                 # L7.5: vendor/ promoted from PRIV_INCLUDE_DIRS to public
                 # INCLUDE_DIRS so nanojs8_ft8_lib (which REQUIRES us) can
                 # find <kissfft/kiss_fftr.h>. Single KissFFT in the build.
                 "vendor"
                 "vendor/kissfft"
                 # L7.7: src/ promoted to public so nanojs8_js8_codec can
                 # find Varicode.h for the protocol-layer message unpack
                 # (unpackHeartbeatMessage, unpackDirectedMessage, etc.).
                 # These are the public protocol-layer headers; lower-level
                 # headers like commons.h / log.h are not meant to be
                 # consumed externally but their presence is harmless.
                 "src"
    REQUIRES     esp_common
    PRIV_REQUIRES
                 log
                 esp_psram        # EXT_RAM_BSS_ATTR
                 esp_timer        # L7.11h.1: esp_timer_get_time in JSC::init()
                 freertos         # L7.11h.1-fix1: taskYIELD during JSC init
                 nanojs8_jsc_map  # jsc_map.h, nanojs8_jsc_map_word()
)

# C++20 is REQUIRED for gfsk8 — uses std::span, <numbers>, <concepts>.
# Set on the component target only; doesn't affect other components.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# Silence vendor-source warnings that aren't actionable for us
# (we don't own this code's style). Stay strict on our own code.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-misleading-indentation
)

# KissFFT vendored files use C, not C++ — suppress their warnings too.
set_source_files_properties(
    vendor/kissfft/kiss_fft.c
    vendor/kissfft/kiss_fftr.c
    PROPERTIES COMPILE_FLAGS "-w"
)
