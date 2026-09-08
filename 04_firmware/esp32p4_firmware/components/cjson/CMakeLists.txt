# 上游 cJSON（MIT）——与 ESP-IDF 内置组件同名同源，
# 覆盖内置版以让固件与宿主机测试跑同一份代码。
# 编译期的 "component overridden" 提示属预期。
idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")
