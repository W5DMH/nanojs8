idf_component_register(
    SRCS        "src/tx_queue.c"
    INCLUDE_DIRS "include"
    REQUIRES    esp_common freertos
    PRIV_REQUIRES log esp_timer nanojs8_tx_audio
                  esp_psram   # L7.16-fix2: MALLOC_CAP_SPIRAM for drain task PSRAM stack
)
