# NanoJS8 — nanojs8_ui component
#
# Owns the input event taxonomy, the M5Cardputer keysState translator, the
# screen router state machine, and the concrete screens. Phase 2 ships the
# full 7-screen ring: HOME (real), SETUP (real, from Phase 1), and 5
# placeholders (HEARD, DIRECTED, INBOX, COMPOSE, ALLCALL) using a single
# parameterized PlaceholderScreen class.

idf_component_register(
    SRCS
        "input_translator.cpp"
        "screen_router.cpp"
        "screens/home_screen.cpp"
        "screens/placeholder_screen.cpp"
        "screens/setup_screen.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        M5Unified
        M5GFX
        M5Cardputer
        nanojs8_config
        nanojs8_radio
        esp_timer
)

# Warnings on our own code, no -Werror (vendored M5* tripped GCC 14 false-
# positives in Phase 0; same policy here).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
