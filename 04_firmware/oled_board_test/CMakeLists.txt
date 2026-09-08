cmake_minimum_required(VERSION 3.16)
# 板测程序复用正式固件的组件（cjson）与 petlink 协议核心——单一事实源，不复制代码
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../esp32p4_firmware/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(oled_board_test)
