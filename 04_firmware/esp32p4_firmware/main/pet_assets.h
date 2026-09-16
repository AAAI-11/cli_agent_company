/**
 * PNG 素材槽头文件——支持按 agent 分目录（agent/status.png）。
 *
 * 约定：
 *   A:/<agent>/<status>.png  例如 A:/codex/thinking.png, A:/claude/working.png
 *   agent="codex-cli" 或 "codex:" 前缀 → 目录 "codex"
 *   agent="claude-code" 或 "claude" → 目录 "claude"
 *   空/未知 agent → 目录 "default"
 *   CONNECTING 复用同目录下的 thinking.png
 *
 * 缺失/失败全程矢量兜底。
 */
#ifndef PET_ASSETS_H
#define PET_ASSETS_H

#include <stdbool.h>

#include "sdkconfig.h"

#include <lvgl.h>

#include "petlink.h"

#if CONFIG_PETLINK_UI_PNG

/** 挂载 asset 分区 + 建隐藏 lv_image */
void pet_assets_init(lv_obj_t *parent);
/** 该态 PNG 是否就位 */
bool pet_assets_has(pl_status_t st);
/** 切换显示：按 agent 选择目录下的 PNG，否则隐藏 */
void pet_assets_show(pl_status_t st, bool on, const char *agent);
/** anim_step 每拍同步宠物中心 */
void pet_assets_move(int cx, int cy);

#else

static inline void pet_assets_init(lv_obj_t *parent) { (void)parent; }
static inline bool pet_assets_has(pl_status_t st) { (void)st; return false; }
static inline void pet_assets_show(pl_status_t st, bool on, const char *agent) { (void)st; (void)on; (void)agent; }
static inline void pet_assets_move(int cx, int cy) { (void)cx; (void)cy; }

#endif

#endif /* PET_ASSETS_H */
