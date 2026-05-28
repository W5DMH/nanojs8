# NanoJS8 — vendored copy of espressif/usb_host_uac v1.3.3
#
# Why vendored:
#   - The IDF Component Manager registry was serving a regression of v1.2.0
#     content where the public callback signature used the reserved word
#     'class' as a parameter name, blocking C++ compilation.
#   - Vendoring fixes that by tracking Mini-FT8's verified-working v1.3.3
#     copy (commit_sha 26f9973b2461f3452f2de03fd6b8c5738014b68d on
#     espressif/esp-usb/host/class/uac/usb_host_uac).
#   - Same model we use for M5Cardputer in this project: control the file
#     contents to eliminate registry-side moving parts.
#
# The upstream component depends on cmake_utilities for the
# cu_pkg_define_version macro. We drop that here — version baking
# isn't needed for our in-tree use, and removing the dependency means
# this component is self-contained.

idf_component_register(
    SRCS
        "uac_descriptors.c"
        "uac_host.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_ringbuf
    REQUIRES
        usb
)

# usb_host_uac's source references USB_HOST_UAC_VER_MAJOR/MINOR/PATCH at
# runtime (in an ESP_LOGI line at install time). Upstream gets these from
# cu_pkg_define_version() (cmake_utilities). We inject them directly.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    USB_HOST_UAC_VER_MAJOR=1
    USB_HOST_UAC_VER_MINOR=3
    USB_HOST_UAC_VER_PATCH=3
)
