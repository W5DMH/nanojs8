# NanoJS8 ? gfsk8_modem component CMakeLists.txt
#
# Phase 0: explicitly disabled. The upstream gfsk8-modem-clean cannot be
# compiled under ESP-IDF without significant porting work that belongs in
# Phase 4:
#   - kissfft dependency must be vendored or replaced
#   - gfsk8modem.h project header must be authored
#   - exception handling must be removed (ESP-IDF defaults to -fno-exceptions)
#   - the two memory patches must be applied
#
# Until Phase 4 lands, this is a registered empty component so the build
# graph stays stable.

idf_component_register()
