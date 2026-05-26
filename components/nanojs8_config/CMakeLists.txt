# NanoJS8 — nanojs8_config component
#
# Owns the persistent configuration: schema, NVS read/write, validation,
# and default population. Public surface: config_schema.h + config_store.h.
#
# Threading note: this component is NOT internally thread-safe. Phase 1's
# only caller is the UI task. Phase 4+ will need a mutex if config edits
# come from BLE/serial.

idf_component_register(
    SRCS "config_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)

# Warnings on our own code, but no -Werror (vendored components fail it on
# GCC 14 false-positives; see M5Unified patch). We catch our own warnings
# by reading the build log; CI will fail on any non-zero idf.py exit anyway.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
