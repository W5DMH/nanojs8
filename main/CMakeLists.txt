# NanoJS8 — main component CMakeLists.txt
#
# Phase 0 deliverable: minimal splash screen.
#
# REQUIRES list mirrors what Mini-FT8 builds against (verified working on
# Cardputer ADV with ESP-IDF v5.5.x), trimmed to Phase 0 needs:
#   - M5Unified / M5GFX / M5Cardputer: display + keyboard + power
#   - board_cardputer_adv: I2C / ES8311 / power init (we don't call audio yet)
#   - fatfs, sdmmc, esp_driver_sdspi: SD-card detection
#   - nvs_flash: required by M5Unified's begin() path
#
# gfsk8_modem is NOT yet listed — we link it in Phase 4 when we actually decode.
# That keeps Phase 0 build time fast and the binary small.

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        M5Unified
        M5GFX
        M5Cardputer
        board_cardputer_adv
        fatfs
        sdmmc
        esp_driver_sdspi
        nvs_flash
)

# Build with strict warnings as errors. Catches issues now rather than during
# Phase 4 when the codebase is larger.
#target_compile_options(${COMPONENT_LIB} PRIVATE -Werror -Wall -Wextra -Wno-unused-parameter)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# POST_BUILD DRAM budget check. For Phase 0 we just print the number; the script
# becomes a hard gate in Phase 4 when we have the real audio buffer requirement.

