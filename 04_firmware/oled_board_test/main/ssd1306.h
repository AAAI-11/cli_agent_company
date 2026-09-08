/**
 * SSD1306 128×64 OLED 硬件层（I2C，ESP-IDF 专用，不进宿主机测试）。
 *
 * 只管三件事：总线、扫描、整帧刷新。显示内容全部来自 oled_ui 的帧缓冲。
 * 命令序列按 SSD1306 数据手册通用初始化流程编写（128×64 / 内部升压）。
 */
#ifndef SSD1306_DRV_H
#define SSD1306_DRV_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define SSD1306_W  128
#define SSD1306_H  64
#define SSD1306_FB_SIZE (SSD1306_W * SSD1306_H / 8)

/** 初始化 I2C 总线（微雪 P4 板载外部上拉，内部上拉不开） */
esp_err_t oled_hal_init(int sda, int scl);

/** 探测 0x08..0x77，应答的地址写入 addrs（≤max），返回找到的数量 */
int oled_hal_scan(uint8_t *addrs, int max);

/** 在总线上挂 SSD1306（addr 七位制，如 0x3C）并完成初始化；flip=1 上下翻转 */
esp_err_t oled_hal_attach(int addr, int hz, int flip);

bool oled_hal_ready(void);

/** 整帧写入（页式帧缓冲直刷） */
esp_err_t oled_hal_flush(const uint8_t *fb);

/* 注意：保护宏不得叫 SSD1306_H——会与高度常量 SSD1306_H 重名 */
#endif /* SSD1306_DRV_H */
