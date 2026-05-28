# nanojs8_radio — Phase 3a component
#
# Provides the USB host plumbing that drives the radio attached via the
# Cardputer ADV's USB-C port (in OTG host mode). Phase 3a supports one
# profile: digirig_unknown (DigiRig + arbitrary radio, RTS-PTT, no CAT).
# More profiles slot in additively in Phase 3b/3c without touching the
# public API in include/radio_service.h.
#
# REQUIRES list rationale:
#   nanojs8_config  — radio profile id read from NVS at start()
#   usb             — base USB host library (manages the OTG controller)
#   usb_host_uac    — UAC (audio) host driver
#   usb_host_cdc_acm/_vcp/_cp210x_vcp — CDC/serial host stack for PTT
#   driver          — for ll register access during PHY switch
#   esp_timer       — millis-equivalent for diagnostics
#   freertos        — task creation + StreamBuffer (RX ring)

idf_component_register(
    SRCS
        "radio_service.cpp"
        "radio_profile.cpp"
        "radio_rx_buffer.cpp"
        "usb_host_task.cpp"
        "uac_manager.cpp"
        "cp2102_ptt.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nanojs8_config
        driver
        esp_timer
        freertos
        soc
    PRIV_REQUIRES
        usb
        usb_host_uac
        espressif__usb_host_cdc_acm
        espressif__usb_host_vcp
        espressif__usb_host_cp210x_vcp
)

# Build with the same strict warnings as the other components — we want
# any uninitialized variable or implicit narrowing caught at compile time.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)
