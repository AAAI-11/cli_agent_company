#!/bin/sh
# PetLink 核心宿主机测试：与 ESP-IDF 完全无关，秒级跑完。
# 用法：sh test/run.sh
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -Wextra -Wno-unused-parameter \
   -I../main -I../components/cjson \
   test_petlink.c ../main/petlink.c ../components/cjson/cJSON.c \
   -o /tmp/pl_test
/tmp/pl_test
cc -O2 -Wall -Wextra -Wno-unused-parameter \
   -I../main \
   test_ui_map.c ../main/ui_map.c \
   -o /tmp/ui_map_test
/tmp/ui_map_test
cc -O2 -Wall -Wextra -Wno-unused-parameter \
   -I../main \
   test_joystick.c ../main/joystick.c \
   -o /tmp/joy_test
/tmp/joy_test
