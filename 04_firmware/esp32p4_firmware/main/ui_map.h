/**
 * 状态 → UI 样式映射（纯 C，零 LVGL 依赖，宿主机可测）。
 * 颜色/标签与 virtual_device/pet.js 的 STATE_STYLE 同源（Tokyo Night）；
 * CONNECTING 是 pet.js 初始 footer（"正在连接 daemon…"）心智的实体化：
 * 注释灰 + 呼吸闪烁。
 * 动画族由 pet_ui.c 解释执行，这里只出枚举——保证映射表可单测。
 */
#ifndef UI_MAP_H
#define UI_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "petlink.h"

typedef enum {
  UI_ANM_BLINK = 0, /* CONNECTING：状态条呼吸 */
  UI_ANM_DOTS,      /* THINKING：省略号循环 */
  UI_ANM_ARC,       /* WORKING：进度弧 */
  UI_ANM_BOUNCE,    /* WAITING_CONFIRM：急促点头 + ! */
  UI_ANM_STARS,     /* DONE：星星环绕 */
  UI_ANM_SHAKE,     /* ERROR：抖动 + 汗滴 */
  UI_ANM_BOB,       /* 通用：身体慢起伏（叠加在所有态上） */
} ui_anim_t;

typedef struct {
  uint32_t    fg;    /* 0xRRGGBB，Tokyo Night */
  const char *label; /* 中文短标签（UTF-8，宪法 F01） */
  ui_anim_t   anim;  /* 该态的主动画族 */
  const char *sample_text; /* SELFTEST 轮播的 text 演示值（⊆ PET_FONT_TEXT_SYMBOLS） */
} ui_style_t;

/** 查表；越界/未知回退 CONNECTING 项（前向兼容语义） */
const ui_style_t *ui_style_for(pl_status_t st);

/**
 * text 短标签显示口径（pet.js L67/L266 同源）：
 * text 为空/NULL → 回退状态中文名；非空 → 截前 16 个码点（UTF-8 不劈半字）。
 * 结果写入 out（至多 cap 字节，含 NUL）；cap 为 0 时直接返回。
 */
void ui_text_line(const char *text, pl_status_t st, char *out, size_t cap);

/**
 * 字体子集符号集：六个标签的汉字并集 + ★！…。
 * ⚠️ main/font_pet_32.c 生成时的 --symbols 必须与这里逐字一致，
 * 缺字会显示 '?'（宿主测试锚定，OLED 时代的老坑）。
 * text 短标签字体（font_pet_text_24）的符号集见 font_text_symbols.h
 * （tools/gen_font_text.py 生成，同样宿主测试锚定）。
 */
#define PET_FONT_SYMBOLS "思考中干活等你拍板完成了出错正在连接★！…"

#endif /* UI_MAP_H */
