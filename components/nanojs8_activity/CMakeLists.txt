# nanojs8_activity — L7.9 in-memory "who did we hear" + "what was said"
# =====================================================================
# Two thread-safe tables maintained by the js8_sync task as decode
# events arrive, queried by the UI task to paint the HEARD and
# DIRECTED screens. All storage is static (no heap), and a single
# FreeRTOS mutex serialises access.

idf_component_register(
    SRCS
        "src/activity.c"
        "src/grid_math.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        log
        esp_timer       # esp_timer_get_time for monotonic "age" tracking
        freertos        # SemaphoreHandle_t, xSemaphoreCreateMutex
        nanojs8_config  # operator's grid → distance/bearing math
        nanojs8_time    # seconds-today for HH:MM:SS at-record stamps
)
