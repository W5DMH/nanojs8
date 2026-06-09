# nanojs8_usb_audio — Layer 5 USB Audio Class 1.0 host
#
# REQUIRES (public, via audio.h):
#   (none — public header is plain stdint + esp_err_t which are standard)
#
# PRIV_REQUIRES (implementation-only deps):
#   usb              — ESP-IDF USB Host library (usb_host_install,
#                      usb_host_lib_handle_events, etc.)
#   espressif__usb_host_uac — managed dep for the UAC class driver
#                      (uac_host_install, uac_host_device_*, ...)
#   log, esp_common  — ESP_LOG family and esp_err_to_name
idf_component_register(
    SRCS "audio.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usb espressif__usb_host_uac log esp_common
)
