# nanojs8_config — Layer 6b.1 persistent operator config
#
# REQUIRES (public, via config.h):
#   (none — public header is stdint + esp_err_t)
#
# PRIV_REQUIRES (implementation):
#   nvs_flash       — nvs_flash_init, nvs_open, nvs_get/set_str, nvs_commit
#   log, esp_common — ESP_LOG, esp_err_to_name
idf_component_register(
    SRCS "config.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash log esp_common
)
