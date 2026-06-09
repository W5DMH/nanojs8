# nanojs8_js8_sync — JS8 Normal-mode sync detection (L7.5)
#
# Bridges nanojs8_rx_audio's slot-aligned 12 kHz int16 audio to ft8_lib's
# monitor/sync chain. With the Costas pattern in nanojs8_ft8_lib's
# constants.c swapped to JS8 Normal, the FT8 sync detector becomes a
# JS8 sync detector. This layer is sync-only — no LDPC/decode/payload.

idf_component_register(
    SRCS         "src/js8_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_common nanojs8_rx_audio nanojs8_time
    PRIV_REQUIRES
                 log
                 esp_timer
                 esp_psram
                 nanojs8_ft8_lib
                 nanojs8_js8_codec   # L7.6: LDPC + CRC + message extract
                 nanojs8_activity    # L7.9: HEARD + DIRECTED ingest
                 nanojs8_config      # L7.11g.4: my callsign + groups CSV
                 nanojs8_mailbox     # L7.11g.4: add_unread on MSG verb to-me
                 nanojs8_tx_queue    # L7.11g.4: auto-ACK enqueue
)
