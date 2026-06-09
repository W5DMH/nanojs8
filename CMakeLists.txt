# ============================================================================
# NanoJS8 v0.7 — Cardputer→T-Deck port
# Layer 6b.5: Icom CI-V CAT for the Xiegu G90 over DigiRig
# ============================================================================
#
# Build:
#   & "C:\esp\v5.5.4\esp-idf\export.ps1"
#   cd C:\dev\nanojs8_v07_l6b4
#   idf.py set-target esp32s3
#   idf.py build
#
# Flash:
#   idf.py -p COMxx flash
#
# Monitor (from Pi via Grove):
#   sudo screen /dev/ttyAMA0 115200
#
# Author: W5DMH (dan@hurdfarms.com)
# License: GPL-3.0
# ============================================================================

cmake_minimum_required(VERSION 3.18)   # set_source_files_properties DIRECTORY requires 3.18+

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Use MINIMAL_BUILD to limit the build set to only components we actually
# need (and their transitive deps). This:
#   1. Cuts the binary size meaningfully — we skip many components that
#      common-build would include by default (esp_eth, mqtt, http_client,
#      esp_https_ota, etc.). NanoJS8 will add what it needs explicitly via
#      its component REQUIRES lists.
#   2. Speeds up builds.
#
# When new components are needed (e.g. LovyanGFX in Layer 3, USB in
# Layer 5), they get added via the appropriate component's
# idf_component_register(REQUIRES ...) — NOT here.
idf_build_set_property(MINIMAL_BUILD ON)

project(nanojs8)

# ─────────────────────────────────────────────────────────────────────────
# GCC 14.2.0 ICE workaround for esp_lcd_panel_rgb.c
# ─────────────────────────────────────────────────────────────────────────
# The Xtensa GCC 14.2.0 (build 20260121) toolchain hits an internal
# compiler error during the IRA (Integrated Register Allocator) pass on
# C:/esp/v5.5.4/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c.
# Reproducible on a clean rebuild; ccache had been hiding it across
# Layers 1-5b by serving a previously-cached .o file. Layer 6b.1 added
# new components, which slightly changed the build dependency graph and
# invalidated ccache for this file, exposing the bug.
#
# The IRA pass is only active at -O1 and above. Dropping this specific
# file to -O0 dodges the broken pass entirely. We never use the RGB
# panel driver — nanojs8_display uses the SPI ST7789 driver in the same
# component, so the RGB code path is dead at runtime. Zero functional
# impact, just defeats ccache misses.
#
# Why this is here (top-level) and not in nanojs8_display:
# set_source_files_properties with the DIRECTORY scope must be set in
# a directory that's a parent of, or equal to, the directory where the
# source is registered. esp_lcd_panel_rgb.c is registered in IDF's own
# components/esp_lcd/CMakeLists.txt. Setting the property here, after
# project(), lets us reach into IDF's component tree without modifying
# anything in $IDF_PATH (which would be lost on toolchain re-install).
#
# Verification: after build, the .o file should still link cleanly; the
# RGB driver functions just won't be optimized. We never call them, so
# they're dead-stripped at link time anyway (-ffunction-sections is in
# IDF's default flags).
set(_rgb_buggy_file
    "$ENV{IDF_PATH}/components/esp_lcd/rgb/esp_lcd_panel_rgb.c")
if(EXISTS "${_rgb_buggy_file}")
    set_source_files_properties(
        "${_rgb_buggy_file}"
        DIRECTORY "$ENV{IDF_PATH}/components/esp_lcd"
        PROPERTIES COMPILE_OPTIONS "-O0"
    )
    message(STATUS
        "Workaround applied: -O0 for esp_lcd_panel_rgb.c "
        "(GCC 14.2.0 IRA-pass ICE)")
else()
    message(WARNING
        "esp_lcd_panel_rgb.c not found at expected path — workaround "
        "skipped. If the build fails with an IRA-pass ICE, locate the "
        "file under \$IDF_PATH and update the path in CMakeLists.txt.")
endif()
unset(_rgb_buggy_file)

# ─────────────────────────────────────────────────────────────────────────
# JSC map binary blob — L7.4a
# ─────────────────────────────────────────────────────────────────────────
# JS8's source codec needs a 262,144-entry word dictionary (JSC::map).
# Compiling it inline would cost ~3 MB of the app partition. Instead we
# build a memory-mappable binary from the upstream JSC_map.cpp source
# and flash it to a dedicated `jsc_map` partition (6 MB, see
# partitions.csv). At runtime, components/nanojs8_jsc_map locates the
# partition and mmap's it via esp_partition_mmap.
#
# This block:
#   1. Tells CMake how to run tools/pack_jsc_map.py to produce
#      jsc_map.bin from assets/JSC_map.cpp.
#   2. Hooks the resulting .bin into `idf.py flash` so the partition is
#      written along with the app.
#   3. Adds a hard build-time dependency on the source + script so any
#      change to either re-packs.

set(JSC_MAP_SRC    "${CMAKE_CURRENT_SOURCE_DIR}/assets/JSC_map.cpp")
set(JSC_MAP_BIN    "${CMAKE_BINARY_DIR}/jsc_map.bin")
set(JSC_MAP_TOOL   "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_jsc_map.py")

if(NOT EXISTS "${JSC_MAP_SRC}")
    message(FATAL_ERROR
        "JSC_map.cpp not found at ${JSC_MAP_SRC}. Copy it from "
        "gfsk8-modem-clean/src/JSC_map.cpp into assets/.")
endif()
if(NOT EXISTS "${JSC_MAP_TOOL}")
    message(FATAL_ERROR
        "pack_jsc_map.py not found at ${JSC_MAP_TOOL}. The tool is "
        "shipped in tools/ — re-extract the build zip.")
endif()

# Custom command: run the packer when the source or the tool changes.
# Uses Python from the IDF env (set by export.ps1/.sh as `python`).
add_custom_command(
    OUTPUT "${JSC_MAP_BIN}"
    COMMAND "${python}" "${JSC_MAP_TOOL}" "${JSC_MAP_SRC}" "${JSC_MAP_BIN}"
    DEPENDS "${JSC_MAP_SRC}" "${JSC_MAP_TOOL}"
    COMMENT "Packing JSC dictionary (262,144 entries → jsc_map.bin)"
    VERBATIM
)
add_custom_target(jsc_map_bin ALL DEPENDS "${JSC_MAP_BIN}")

# Wire into flash. esptool_py_flash_to_partition() registers the binary
# with the named partition; `idf.py flash` writes both app and JSC blob.
# Available since ESP-IDF 5.x.
esptool_py_flash_to_partition(flash "jsc_map" "${JSC_MAP_BIN}")
add_dependencies(flash jsc_map_bin)

# Also hook into `idf.py app-flash` — useful when iterating on the
# dictionary without re-flashing the app.
if(TARGET app-flash)
    add_dependencies(app-flash jsc_map_bin)
endif()


