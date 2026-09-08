/**
 * 五态 PNG 素材槽（v0.3）：asset 分区（2MB FAT，长文件名）+ lodepng。
 *
 * 约定与 virtual_device/assets/README.md 完全一致（美术不做两遍）：
 * thinking/working/waiting_confirm/done/error.png，352×352（显示 176，
 * 2 倍图保清晰）。CONNECTING 复用 thinking.png——浏览器初始态同款
 * （pet.js 无 connecting 态，缺省落 thinking）。
 *
 * 兜底铁律：挂载失败 / 缺图 / 解码失败都回退矢量小狗（pet_ui 内建），
 * 演示永不因缺素材断链。Kconfig PETLINK_UI_PNG=n 时全部内联成空操作。
 */
#ifndef PET_ASSETS_H
#define PET_ASSETS_H

#include <stdbool.h>

#include "sdkconfig.h"

#include <lvgl.h>

#include "petlink.h"

#if CONFIG_PETLINK_UI_PNG

/** 挂载 asset 分区 + 建隐藏 lv_image（build_page 在光晕之后调用，定 z 序） */
void pet_assets_init(lv_obj_t *parent);
/** 该态 PNG 是否就位（挂载失败恒 false → 矢量兜底） */
bool pet_assets_has(pl_status_t st);
/** 切换显示：on 且素材在则换 src 并显示，否则隐藏（矢量侧自行接管） */
void pet_assets_show(pl_status_t st, bool on);
/** anim_step 每拍同步宠物中心（bob/shake 与矢量狗同相位） */
void pet_assets_move(int cx, int cy);

#else /* PNG 槽关闭：内联空操作，pet_ui 调用点零改动 */

static inline void pet_assets_init(lv_obj_t *parent) { (void)parent; }
static inline bool pet_assets_has(pl_status_t st) { (void)st; return false; }
static inline void pet_assets_show(pl_status_t st, bool on) { (void)st; (void)on; }
static inline void pet_assets_move(int cx, int cy) { (void)cx; (void)cy; }

#endif /* CONFIG_PETLINK_UI_PNG */

#endif /* PET_ASSETS_H */
