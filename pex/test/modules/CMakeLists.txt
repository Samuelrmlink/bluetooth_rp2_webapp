# Standalone build of the PEX test modules (no firmware):
#   cmake -S pex/test/modules -B build-pexmods -G Ninja && cmake --build build-pexmods
cmake_minimum_required(VERSION 3.20)
project(pex_test_modules NONE)
include(${CMAKE_CURRENT_LIST_DIR}/modules.cmake)
