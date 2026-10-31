cmake_minimum_required(VERSION 3.16)

# The running app reports this string in image state-read. An update has to
# be strictly newer or state-write will not arm it. -DBTGW_PROJECT_VER=1.0.1
# builds the next image without editing this file.
if(NOT BTGW_PROJECT_VER)
    set(BTGW_PROJECT_VER "1.0.0")
endif()
set(PROJECT_VER "${BTGW_PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btgw-esp32c5)
