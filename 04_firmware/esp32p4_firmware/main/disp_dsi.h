/**
 * MIPI-DSI 屏幕点亮（微雪 2.8inch DSI LCD，480×640，ST7701）。
 * 调用序列与本地官方例程同构（宪法 L109：底座采用 Espressif 官方示例）：
 *   LDO → DSI bus → DBI IO → ST7701 面板 → reset/init → 背光。
 * 时序参数来自微雪 BSP 实测表（waveshare/esp_lcd_dsi MIT 组件 README，
 * 2.8inch 行标注 tested YES——硬件事实参数，非代码拷贝）。
 */
#ifndef DISP_DSI_H
#define DISP_DSI_H

#include "esp_lcd_panel_ops.h"

/** 全套点亮：LDO→总线→面板→init→背光。失败直接 ESP_ERROR_CHECK 重启打日志。 */
esp_lcd_panel_handle_t disp_dsi_init(void);

/** B1 阶段验收：DSI 控制器硬件彩条（不经过 framebuffer/LVGL） */
void disp_dsi_color_bars(esp_lcd_panel_handle_t panel);

#endif /* DISP_DSI_H */
