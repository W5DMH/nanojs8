# NanoJS8 v0.7 — main component
#
# Layer 1: Just main.c. No platform component yet, no display, no USB host.
# Those layers add their own components with their own REQUIRES.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # Components we directly use in main.c:
    #   esp_psram              — esp_psram_is_initialized(), get_size()
    #   esp_hw_support         — esp_chip_info()
    #   spi_flash              — esp_flash_get_size()
    #   nanojs8_platform_tdeck — tdeck_platform_init()  (Layer 2)
    #   nanojs8_display        — nanojs8_display_* (Layer 3)
    #   nanojs8_keyboard       — nanojs8_keyboard_* (Layer 4)
    #   nanojs8_usb_audio      — nanojs8_audio_* (Layer 5)
    #   nanojs8_usb_serial     — nanojs8_serial_* (Layer 5b, DigiRig CAT/PTT)
    #   nanojs8_trackball      — nanojs8_trackball_* (Layer 6b.1)
    #   nanojs8_config         — nanojs8_config_* (Layer 6b.1)
    #   nanojs8_ui             — nanojs8_ui_* (Layer 6b.2)
    #   nanojs8_radio          — nanojs8_radio_* (Layer 6b.4 profile registry)
    #   nanojs8_ptt            — nanojs8_ptt_* (Layer 6b.4 PTT controller)
    #   nanojs8_cat            — nanojs8_cat_* (Layer 6b.5 CAT facade)
    #   nanojs8_cat_civ        — Icom CI-V framing (Layer 6b.5)
    #   nanojs8_time           — nanojs8_time_* (Layer 7.0 manual UTC clock)
    #   nanojs8_rx_audio       — nanojs8_rx_audio_* (Layer 7.1 RX pipeline)
    #   nanojs8_jsc_map        — memory-mapped JSC dictionary loader (L7.4a)
    #   nanojs8_gfsk8          — vendored gfsk8 JS8 modem (L7.4b); we keep
    #                            it built (we'll lift bpdecode174 + Varicode
    #                            + JSC.cpp from here in L7.6+). Also exposes
    #                            kissfft headers used by nanojs8_ft8_lib.
    #   nanojs8_ft8_lib        — Mini-FT8's monitor + sync chain, Costas
    #                            constants swapped to JS8 Normal (L7.5)
    #   nanojs8_js8_sync       — bridges rx_audio slots into ft8_lib for
    #                            JS8 Normal sync detection (L7.5); extended
    #                            in L7.6 to also run the LDPC decode pipeline
    #                            and extract 12-character raw messages.
    #   nanojs8_js8_codec      — JS8 LDPC + CRC-12 + message extract (L7.6),
    #                            with bpdecode174 + Mn/Nm + CRC12 lifted
    #                            verbatim from gfsk8, and our own LLR
    #                            extractor (natural-binary, no Gray code).
    # IMPORTANT: in MINIMAL_BUILD mode, only components that appear here
    # (transitively) get enrolled. New components MUST be added here or
    # they'll be silently dropped from the build. (Lesson from L5b.)
    # nanojs8_cat_civ is in the list explicitly even though it's only
    # consumed by nanojs8_cat — keeps the dependency graph visible.
    # L7.4b: removed nanojs8_ft8_lib + old nanojs8_decoder (FT8 RX retired
    # in favor of gfsk8 for the JS8 protocol).
    # L7.4c: added nanojs8_js8_decoder (gfsk8::Decoder bridge — FAILED
    # with std::bad_alloc; the variant-of-DecodeMode<> was ~85 MB).
    # L7.5: removed nanojs8_js8_decoder; brought ft8_lib back (renamed
    # nanojs8_ft8_lib) with JS8 Costas swap; added nanojs8_js8_sync.
    # L7.6: added nanojs8_js8_codec — full RX message decode now wired.
    REQUIRES esp_psram esp_hw_support spi_flash
             driver
             nanojs8_gps
             nanojs8_platform_tdeck nanojs8_display nanojs8_keyboard
             nanojs8_usb_audio nanojs8_usb_serial
             nanojs8_trackball nanojs8_config nanojs8_ui
             nanojs8_radio nanojs8_ptt
             nanojs8_cat nanojs8_cat_civ nanojs8_time nanojs8_rx_audio
             nanojs8_jsc_map nanojs8_gfsk8
             nanojs8_ft8_lib nanojs8_js8_sync nanojs8_js8_codec
             nanojs8_activity        # L7.9: HEARD + DIRECTED data store
             nanojs8_tx_audio        # L7.11c: 48k stereo pre-render + USB-TX worker
             nanojs8_mailbox         # L7.11f: COMPOSE→STORE mailbox
             nanojs8_tx_queue        # L7.11g.4: 4-deep FIFO for queued short-form TX
)
