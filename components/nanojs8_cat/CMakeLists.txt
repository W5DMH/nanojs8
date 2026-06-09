idf_component_register(
    SRCS "cat.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nanojs8_radio        # public — cat.h includes radio.h
    PRIV_REQUIRES
        log
        esp_common           # for esp_err_t — declared in cat.h
        esp_timer            # for esp_timer_get_time() in cat.cpp
        nanojs8_cat_civ      # CI-V protocol library
        nanojs8_usb_serial   # for the RX callback + serial write
)
