# NanoJS8 — main component CMakeLists.txt (Phase 1)
#
# Phase 1 adds two project-owned components to the dependency graph:
#   - nanojs8_config: NVS-backed configuration
#   - nanojs8_ui:     input translator + screen router + screens
#
# These transitively pull in M5Cardputer/M5Unified/M5GFX (via nanojs8_ui)
# and nvs_flash (via nanojs8_config), so we don't need to list them here.

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nanojs8_config
        nanojs8_ui
        M5Unified
        M5GFX
        M5Cardputer
        board_cardputer_adv
        fatfs
        sdmmc
        esp_driver_sdspi
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
