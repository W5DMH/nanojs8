idf_component_register(
    SRCS "ptt.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nanojs8_radio nanojs8_usb_serial
                  esp_common esp_timer freertos log
)
