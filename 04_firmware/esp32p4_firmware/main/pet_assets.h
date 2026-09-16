/**
 * PNG 素材槽头文件——支持按 agent 区分（平铺文件名，fatfsgen 兼容）。
 *
 * 文件名约定：
 *   A:/<agent>_<status>.png  例如 A:/codex_thinking.png, A:/claude_working.png
 *   agent="codex-cli" 或 "codex:" 前缀 → 前缀 "codex"
 *   agent="claude-code" 或 "claude" → 前缀 "claude"
 *   空/未知 agent → 前缀 "default"
 *   CONNECTING 复用同前缀的 _thinking.png
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

void pet_assets_init(lv_obj_t *parent);
bool pet_assets_has(pl_status_t st);
bool pet_assets_show(pl_status_t st, bool on, const char *agent);
void pet_assets_move(int cx, int cy);

#else

static inline void pet_assets_init(lv_obj_t *parent) { (void)parent; }
static inline bool pet_assets_has(pl_status_t st) { (void)st; return false; }
static inline bool pet_assets_show(pl_status_t st, bool on, const char *agent) { (void)st; (void)on; (void)agent; return false; }
static inline void pet_assets_move(int cx, int cy) { (void)cx; (void)cy; }

#endif

#endif /* PET_ASSETS_H */
