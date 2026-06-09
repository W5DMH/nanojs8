# nanojs8_keyboard — Layer 4 ESP32-C3 keyboard reader (I²C 0x55)
#
# REQUIRES (public, exposed via keyboard.h):
#   (none — public header is just FreeRTOS types which are always available)
#
# PRIV_REQUIRES (implementation-only deps):
#   esp_driver_i2c         — i2c_master_bus_add_device, i2c_master_receive
#   log                    — ESP_LOG family
#   esp_common             — esp_err_to_name
#   esp_timer              — L6b.6: esp_timer_get_time() for alive timestamp
#   nanojs8_platform_tdeck — i2c_bus() handle and I2C_ADDR_KEYBOARD
idf_component_register(
    SRCS "keyboard.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c log esp_common esp_timer nanojs8_platform_tdeck
)
