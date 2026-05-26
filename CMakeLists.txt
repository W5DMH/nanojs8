# NanoJS8 — Project root CMakeLists.txt
# Minimum CMake version: ESP-IDF v5.5 requires 3.16+. We pin 3.20 to match
# gfsk8-modem-clean's CMakeLists, since the modem is the strictest dependency.
cmake_minimum_required(VERSION 3.20)

# Pull in the standard ESP-IDF project boilerplate. IDF_PATH must be set in
# the shell environment (export.ps1 / export.sh handles this).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name. The output binaries will be nanojs8.bin, nanojs8.elf, etc.
project(nanojs8)
