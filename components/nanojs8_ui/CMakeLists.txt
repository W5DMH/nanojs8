# nanojs8_ui — Layer 6b.2 multi-screen UI framework
#
# REQUIRES (public, via ui.h):
#   (none — public header is stdint + esp_err_t)
#
# PRIV_REQUIRES (implementation):
#   The UI integrates with most of the rest of the firmware to render
#   its status rows. Every component the screens query needs to be in
#   here so the UI's translation units can include those headers and
#   call into those modules.
#
#   nanojs8_display          — drawing primitives
#   nanojs8_config           — current config, is_configured()
#   nanojs8_trackball        — direction constants, counters
#   nanojs8_keyboard         — total_keys() for HOME's keyboard row
#   nanojs8_usb_audio        — stream info for HOME's audio row
#   nanojs8_usb_serial       — port info for HOME's serial row
#   nanojs8_radio            — profile registry for SETUP picker (L6b.4)
#   nanojs8_ptt              — PTT control for HOME hotkeys (L6b.4)
#   nanojs8_cat              — CAT facade for HOME freq row + F hotkey (L6b.5)
#   nanojs8_tx_audio         — L7.11c: 'X' hotkey triggers test-TX audio path
#   log, esp_common          — ESP_LOG, esp_err_to_name
idf_component_register(
    SRCS "ui.cpp" "screen_home.cpp" "screen_setup.cpp"
         "setup_validators.cpp"
         "screen_heard.cpp"           # L7.9
         "screen_all.cpp"             # L7.10: full activity log
         "screen_directed.cpp"        # L7.9, filtered in L7.10
         "screen_compose.cpp"         # L7.11f: multi-field TX form (10 verbs)
         "screen_allcall.cpp"         # L7.11e: TX menu (HB/QUERY/CQ)
         "screen_inbox.cpp"           # L7.11g.3: mailbox list
         "screen_inbox_detail.cpp"    # L7.11g.3: modal detail view
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nanojs8_display nanojs8_config nanojs8_trackball
                  nanojs8_keyboard nanojs8_usb_audio nanojs8_usb_serial
                  nanojs8_radio nanojs8_ptt nanojs8_cat nanojs8_time
                  nanojs8_activity                  # L7.9: HEARD/DIRECTED data
                  nanojs8_tx_audio                  # L7.11c: 'X' test-TX hotkey
                  nanojs8_mailbox                   # L7.11f: COMPOSE→STORE
                  nanojs8_gps                       # L7.14: HOME GPS row
                  log esp_common esp_timer          # L7.9 fix1: esp_timer.h for row-age
)
