# nanojs8_display — Layer 3 ST7789 display driver wrapping esp_lcd
#
# REQUIRES (public, exposed via display.h):
#   (none — public header uses only stdint/stdbool/esp_err which are
#    standard build deps)
#
# PRIV_REQUIRES (used only in implementation):
#   esp_lcd            — esp_lcd_new_panel_io_spi, esp_lcd_new_panel_st7789,
#                        esp_lcd_panel_* operations
#   esp_driver_spi     — spi_bus_initialize
#   log                — ESP_LOG family
#   esp_common         — esp_err_to_name, ESP_CHECK family
#   nanojs8_platform_tdeck — pin constants from platform_tdeck.h
idf_component_register(
    SRCS "display.cpp" "font_8x16.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_lcd esp_driver_spi log esp_common nanojs8_platform_tdeck
)
