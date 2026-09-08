# petlink 协议核心直接引用正式固件源码（改一处两边同步）；
# esp32p4_firmware 是本项目（oled_board_test）的同级目录，从 main/ 要退两级
set(PETLINK_DIR ${CMAKE_CURRENT_LIST_DIR}/../../esp32p4_firmware/main)

idf_component_register(
    SRCS "main.c" "ssd1306.c" "oled_ui.c" "${PETLINK_DIR}/petlink.c"
    INCLUDE_DIRS "." "${PETLINK_DIR}"
    PRIV_REQUIRES driver esp_hw_support esp_timer esp_system spi_flash esp_psram cjson
)
