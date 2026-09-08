#!/bin/sh
# OLED 渲染层宿主机测试：与 ESP-IDF 完全无关，秒级跑完。
# 用法：sh test/run.sh
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -Wextra -Wno-unused-parameter \
   -I../main -I../../esp32p4_firmware/main \
   test_oled_ui.c ../main/oled_ui.c \
   -o /tmp/oui_test
/tmp/oui_test
