# nanojs8_ft8_lib — vendored Mini-FT8 ft8_lib, retuned for JS8 Normal sync.
#
# This component is the "PHY sync engine" half of NanoJS8 v0.7 L7.5+. It
# provides FT8/JS8-style waterfall analysis and Costas-pattern sync
# detection. The library is functionally an FT8 decoder; we use only
# its monitor + sync-search stages (everything up to but not including
# LDPC decode), because:
#   - JS8 frame timing is identical to FT8 (15s slot, 79 sym, 0.160 s/sym).
#   - JS8 Costas sync pattern differs from FT8 — we swap it in constants.c.
#   - JS8 LDPC matrices differ from FT8 — for that we'll use the more
#     memory-efficient bpdecode174 lifted from gfsk8 (L7.6+).
#
# Upstream: https://github.com/Y4HL/Mini-FT8 (components/ft8_lib/)
# Original ft8_lib: Karlis Goba (YL3JG), CC0 / MIT-style.
# License: same as upstream; modifications kept minimal and documented
# inline (search for "NANOJS8 MODIFICATION").
#
# Excluded from the lift:
#   - fft/ — Mini-FT8 bundled KissFFT. We use the one already vendored in
#            nanojs8_gfsk8 (single KissFFT in the build, no symbol clash).
#   - common/stpcpy_compat.c — _GNU_SOURCE in our build already supplies
#            stpcpy at link time (L7.3 lesson). The .h is kept for any
#            translation unit that includes it; the .c is excluded.
#   - common/audio.c, wave.c — host-side WAV I/O, not relevant on MCU.
#
# Layer scope:
#   L7.5: sync detection (monitor + ftx_find_candidates) — this layer
#   L7.6: lift bpdecode174 from gfsk8 → message bit extraction
#   L7.7: Varicode + JSC → human-readable JS8 text

idf_component_register(
    SRCS
        # Sync + waterfall (the part we USE at L7.5):
        common/monitor.c
        common/fft_wrapper.c
        ft8/decode.c              # ftx_find_candidates lives here
        ft8/constants.c           # MODIFIED: Costas pattern → JS8

        # Compiled but not yet consumed (kept for completeness; small);
        # will be replaced/augmented in L7.6+ with JS8-specific paths.
        ft8/crc.c
        ft8/encode.c
        ft8/ldpc.c
        ft8/message.c
        ft8/text.c

    INCLUDE_DIRS
        "."
        "ft8"
        "common"

    REQUIRES
        nanojs8_gfsk8    # Brings in kissfft headers (vendor/kissfft/)
        esp_psram        # heap_caps_malloc(MALLOC_CAP_SPIRAM) in monitor.c
        esp_common       # esp_log, etc.
)

# Mini-FT8 has a few const-static globals declared but not all referenced
# in our reduced surface, and snprintf format-truncation warnings on
# message.c (heavy macro use). Relax these to keep -Werror builds happy
# without touching upstream source.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-const-variable
    -Wno-format
    -Wno-format-truncation
)
