# nanojs8_rx_audio — RX audio decimation + ring + slot trigger (Layer 7.1)
#
# REQUIRES (public, exposed via rx_audio.h):
#   (none — public header is just scalars, esp_err.h)
#
# PRIV_REQUIRES (implementation-only):
#   log               — ESP_LOG family
#   esp_common        — esp_err.h declarations
#   freertos          — task creation, vTaskDelay
#   esp_psram         — heap_caps_malloc for MALLOC_CAP_SPIRAM
#   esp_hw_support    — heap_caps_get_free_size (lives here in v5.5)
#   esp_timer         — transitively via time_source (not used directly)
#   nanojs8_time      — UTC clock for slot timing (millis_today, is_set)
#   nanojs8_usb_audio — read from RX FIFO + check stream state
idf_component_register(
    SRCS "rx_audio.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES log esp_common freertos esp_psram esp_hw_support
                  nanojs8_time nanojs8_usb_audio
)
