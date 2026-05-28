# NanoJS8 — main component CMakeLists.txt (Phase 3a)
#
# Phase 3a additions:
#   - nanojs8_radio: USB host + UAC + CP2102 PTT subsystem
#   - console:        ESP-IDF console library for `radio start/stop/status`
#                     and `ptt on/off` commands over UART0 (the new console)

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nanojs8_config
        nanojs8_ui
        nanojs8_radio
        M5Unified
        M5GFX
        M5Cardputer
        board_cardputer_adv
        fatfs
        sdmmc
        esp_driver_sdspi
        nvs_flash
        console
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
