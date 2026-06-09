idf_component_register(
    SRCS "usb_serial.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        usb
        espressif__usb_host_cdc_acm
        espressif__usb_host_cp210x_vcp
        espressif__usb_host_vcp
        log
        esp_common
)
