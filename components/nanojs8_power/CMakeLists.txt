# nanojs8_power — Phase 3.5 power management component
#
# Owns battery telemetry, charge mode, idle screen dimming, and
# low-battery load-shedding. Depends on:
#   nanojs8_config — settings persistence (idle timeouts, dim level)
#   nanojs8_radio  — to stop the radio service when shedding load
#   M5GFX/M5Unified/M5Cardputer — display brightness/sleep + battery gauge
#   esp_timer      — monotonic clock for the idle timer
#   freertos       — monitor task

idf_component_register(
    SRCS
        "power_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nanojs8_config
        nanojs8_radio
        M5Unified
        M5GFX
        M5Cardputer
        esp_timer
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
