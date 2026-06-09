# nanojs8_gps — L7.14: u-blox MIA-M10Q NMEA reader
#
# Hardware reader for the T-Deck Plus GNSS module. See gps.h for the
# public API and gps.c for design notes.
#
# Conditional behavior is via CONFIG_NANOJS8_GPS_ENABLED in main/Kconfig.projbuild:
#   y → UART1 brought up at runtime, reader task spawned
#   n → init/get_snapshot are no-ops returning DISABLED state
# Parser self-test is always compiled in so the boot self-test
# exercises the code path regardless.
idf_component_register(
    SRCS         "src/gps.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        driver           # UART
        esp_timer        # esp_timer_get_time
        esp_common       # ESP_LOGx
        log
        nanojs8_time     # set_utc_from_gps
)
