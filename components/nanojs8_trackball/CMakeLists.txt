# nanojs8_trackball — Layer 6b.1 directional input component
#
# REQUIRES (public, via trackball.h):
#   (none — public header is stdint + esp_err_t)
#
# PRIV_REQUIRES (implementation):
#   driver           — gpio_config, gpio_isr_handler_add
#   esp_timer        — esp_timer_get_time() for debounce
#   nanojs8_platform_tdeck — PIN_TRACKBALL_* constants from platform_tdeck.h
#   log, esp_common  — ESP_LOG, esp_err_to_name
idf_component_register(
    SRCS "trackball.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer nanojs8_platform_tdeck log esp_common
)
