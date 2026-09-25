cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(CMAKE_PROJECT_mm_wifi_bridge_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/cmake/pio_binary_data.cmake")
project(mm_wifi_bridge)
