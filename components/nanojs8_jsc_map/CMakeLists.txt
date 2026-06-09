# nanojs8_jsc_map — memory-mapped JSC dictionary loader (L7.4a)
#
# Owns the partition lookup, mmap, and accessor for the JS8 source codec
# word table. The actual jsc_map.bin blob is built and flashed by the
# top-level CMake (see project CMakeLists.txt) — this component is purely
# the runtime loader.
#
# REQUIRES:
#   esp_common      — esp_err_t
# PRIV_REQUIRES:
#   log             — ESP_LOG family
#   esp_partition   — esp_partition_find_first / esp_partition_mmap
idf_component_register(
    SRCS         "jsc_map.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_common
    PRIV_REQUIRES log esp_partition
)
