# NanoJS8 ? gfsk8_modem component CMakeLists.txt
#
# Phase 0/1: explicitly disabled. The upstream gfsk8-modem-clean cannot be
# compiled under ESP-IDF without significant porting work that belongs in
# Phase 4 (kissfft vendor, gfsk8modem.h, -fexceptions or strip throws,
# heap-d2 + JSC patches).
#
# Until Phase 4, this is a registered empty component so the build graph
# stays stable and `idf.py build` succeeds.

idf_component_register()
