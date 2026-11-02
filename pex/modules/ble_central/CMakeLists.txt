# Standalone build (m33 and c5):
#   cmake -S pex/modules/ble_central -B $HOME/.cache/btgw-pex-ble/build-central -G Ninja \
#         -DPEX_BLE_OUT=$HOME/.cache/btgw-pex-ble
#   cmake --build $HOME/.cache/btgw-pex-ble/build-central
cmake_minimum_required(VERSION 3.20)
project(pex_ble_central NONE)
include(${CMAKE_CURRENT_LIST_DIR}/ble_central.cmake)
