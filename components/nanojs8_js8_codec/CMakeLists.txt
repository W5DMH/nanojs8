# nanojs8_js8_codec — JS8 LLR extraction + LDPC decode + CRC + message extract
# Layer: L7.6
#         L7.11a — TX encode chain (Varicode pack + gfsk8::encode)
#         L7.11b — TX CPFSK modulator (tones → 12 kHz int16 PCM in PSRAM)
#
# Vendor source for the LDPC/CRC pieces: gfsk8-modem-clean (GPL-3.0).
# Lifted functions: bpdecode174, checkCRC12, extractmessage174-equivalent,
# CRC12() wrapper, Mn + Nm matrices, alphabet, ParityCheckNode struct.
# See ATTRIBUTION.md for details.
#
# LLR extraction (llr_extract.c) is our own implementation modelled on
# ft8_lib's ft8_extract_likelihood but using NATURAL BINARY (no Gray code)
# tone-to-bit mapping per the JS8 spec.
#
# Modulator (js8_modulate.c) is our own pure-C port mirroring gfsk8::modulate
# (CPFSK with continuous phase, no Gaussian shaping despite the gfsk8 name).
# Writes into a pre-allocated PSRAM buffer so the TX path performs zero heap
# operations during transmission.

idf_component_register(
    SRCS
        "src/js8_codec.cpp"
        "src/llr_extract.c"
        "src/msg_assembler.c"   # L7.8: multi-frame message assembly
        "src/js8_encode.cpp"    # L7.11a: TX encoder + self-test
        "src/js8_modulate.c"    # L7.11b: CPFSK modulator → PSRAM
        "src/js8_freetext.c"    # L7.11g.7-fix6: free-text alphabet sanitizer
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        nanojs8_ft8_lib      # ftx_waterfall_t, ftx_candidate_t, WF_ELEM_T
        nanojs8_gfsk8        # L7.7: Varicode protocol-layer unpack
                             # L7.11a: gfsk8::encode + packHeartbeatMessage
    PRIV_REQUIRES
        log
        esp_psram            # L7.11b: heap_caps_malloc(..., MALLOC_CAP_SPIRAM)
        esp_timer            # L7.11f-fix1: pack-cache build-time logging
        freertos             # L7.11f-fix1: pack-cache mutex
        nanojs8_jsc_map      # L7.11h.0: Subtest JSC needs dictionary word lookup
)

# js8_codec.cpp uses C++17 features (std::array, std::string_view, structured
# bindings in some lifted code). C++17 is the default for ESP-IDF v5.5, but
# we set it explicitly to be safe.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
