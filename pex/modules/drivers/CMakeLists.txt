# Standalone build of the driver modules (no firmware):
#   cmake -S pex/modules/drivers -B /tmp/pex-drivers -G Ninja && cmake --build /tmp/pex-drivers
cmake_minimum_required(VERSION 3.20)
project(pex_drivers NONE)
include(${CMAKE_CURRENT_LIST_DIR}/drivers.cmake)
