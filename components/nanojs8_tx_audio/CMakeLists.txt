# nanojs8_tx_audio — L7.11c TX audio path
# ========================================
#
# Pre-renders the 12 kHz mono CPFSK output from nanojs8_js8_codec's
# modulator into a 48 kHz stereo PSRAM buffer (4× linear interpolation
# + mono→stereo duplication) ready to push out the USB UAC TX endpoint
# via nanojs8_audio_write.
#
# Pre-rendering means: zero CPU during the actual USB streaming pass
# (just memcpy'd chunks), deterministic timing, no allocator activity
# on the TX path. The buffer (~2.41 MB) is allocated once at boot.
#
# Hotkey-triggered streaming runs in a one-shot worker task — the
# main UI loop stays responsive while audio plays out. PTT is NOT
# engaged here (that's L7.11d); audio flows to DigiRig but the radio
# does not transmit. Verify by watching the radio's audio meter.

idf_component_register(
    SRCS
        "src/tx_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        freertos
        nanojs8_js8_codec    # modulator sample buffer source
        nanojs8_usb_audio    # nanojs8_audio_write into UAC TX endpoint
        nanojs8_ptt          # L7.11d: nanojs8_ptt_set for keying TX
        nanojs8_radio        # L7.11d: active profile (ptt_on/off_delay_ms)
        nanojs8_time         # L7.11d: millis_today for slot alignment
        nanojs8_config       # L7.11e: read callsign + grid for modulate
    PRIV_REQUIRES
        log
        esp_psram            # heap_caps_malloc(..., MALLOC_CAP_SPIRAM)
        esp_timer            # esp_timer_get_time for elapsed-ms logging
)
