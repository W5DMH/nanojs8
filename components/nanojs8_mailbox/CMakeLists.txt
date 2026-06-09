# nanojs8_mailbox — L7.11g.2 NVS-persistent typed mailbox
# ===================================================================
# Typed message store covering the UNREAD / READ / STORE / DELIVERED
# lifecycle. Backed by a single NVS blob in namespace "nj8_inbox" so
# entries survive reboots. Consumers (current + future layers):
#   - L7.11g.3 INBOX + INBOX_DETAIL UI screens
#   - L7.11g.4 MSG verb auto-ACK
#   - L7.11g.5 MSG TO: store-and-forward
#   - L7.11g.6 QUERY MSGS / QUERY MSG <id> handlers

idf_component_register(
    SRCS "src/mailbox.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        freertos
    PRIV_REQUIRES
        log
        nvs_flash       # L7.11g.2: persistence via single blob in 'nj8_inbox'
        nanojs8_time    # L7.11g.2: nanojs8_time_seconds_today for create stamp
        esp_timer       # L7.16: esp_timer_get_time in persist_worker_task latency log
)
