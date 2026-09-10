idf_component_register(
    SRCS "main.c" "petlink.c" "mic.c" "spk.c" "audio_hw.c" "ui_map.c" "font_pet_32.c" "font_pet_text_24.c" "disp_dsi.c" "pet_ui.c" "pet_assets.c"
    INCLUDE_DIRS "."
    # IDF 6.x：legacy driver 组件已拆分，GPIO/UART/I2S 各归各的 esp_driver_* 组件
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_i2s esp_hw_support esp_timer cjson fatfs
                 esp_lcd esp_mm lvgl espressif__esp_lcd_st7701 esp_driver_i2c espressif__esp_codec_dev
)

# lv_font_conv 生成的字体走 #include "lvgl.h"（组件根目录），
# 而不是默认分支的 "lvgl/lvgl.h"（托管组件目录名是 lvgl__lvgl，解析不到）
add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)

