# nanojs8_time — Manual UTC clock (Layer 7.0)
#
# REQUIRES (public, exposed via time_source.h):
#   (none — public header is just C scalars and esp_err.h)
#
# PRIV_REQUIRES (implementation-only deps):
#   log         — ESP_LOG family
#   esp_common  — esp_err.h declarations
#   esp_timer   — esp_timer_get_time() for the monotonic anchor
idf_component_register(
    SRCS "time_source.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES log esp_common esp_timer
)
