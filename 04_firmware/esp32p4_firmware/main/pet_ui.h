/**
 * 宠物 UI：LVGL 任务 + 快照交接（ui_on_state 的唯一去向）。
 *
 * 任务模型（v0.2 定案 D1）：
 *   - petlink 协议任务永不摸 LVGL 控件，只调 pet_ui_push()（~130B 快照，
 *     互斥锁内 memcpy，临界区 <10µs）
 *   - LVGL 任务独占全部控件，33ms 的 ui_tick 取快照副本驱动界面
 *   - 动画相位在 LVGL 侧用 lv_tick_get() 生成（协议任务零渲染负担）
 *     ——延续 oled_board_test oui_disp_t 快照喂帧的已验证模式
 */
#ifndef PET_UI_H
#define PET_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_panel_ops.h"

#include "petlink.h"

typedef struct {
  pl_status_t state;
  bool link_live;
  char session[16]; /* 显示取前 10 字符（与虚拟设备 slice(0,10) 同口径） */
  char text[100];   /* state 帧短标签（daemon 截 32 cps，最坏 96 UTF-8 字节）；
                       空串=回退状态中文名（与 pet.js 同口径） */
  pl_bubble_t bubbles[PL_MAX_BUBBLES]; /* F02R 多会话气泡（0 个=隐藏该行） */
  uint8_t bubble_n;
  uint32_t arrived_ms; /* pet_ui_push 内盖戳（esp_timer ms），供 <500ms 度量 */
} pet_ui_snap_t;

/** 点亮后调用一次：建 LVGL 任务并接管 panel */
void pet_ui_mount(esp_lcd_panel_handle_t panel);

/** 协议任务调用：推送状态快照（互斥拷贝，绝不阻塞渲染） */
void pet_ui_push(const pet_ui_snap_t *snap);

/** 协议任务调用：链路通断（掉线推回 CONNECTING 由 main.c 负责） */
void pet_ui_link(bool live);

#endif /* PET_UI_H */
