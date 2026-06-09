# nanojs8_platform_tdeck — T-Deck hardware platform layer
#
# REQUIRES are PUBLIC (visible to consumers of this component's headers):
#   esp_driver_i2c     — i2c_master_bus_handle_t in our public header
#
# PRIV_REQUIRES are PRIVATE (only this component's .cpp needs them):
#   esp_driver_gpio    — gpio_config(), gpio_set_level()
#   log                — ESP_LOG family
#   esp_common         — esp_err_to_name(), ESP_CHECK family
#
# (FreeRTOS and esp_err pulled in implicitly by the build's "common" set.)
idf_component_register(
    SRCS "platform_tdeck.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_gpio log esp_common
)
