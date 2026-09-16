/**
 * 五态 PNG 素材槽实现——平铺文件名（fatfsgen.py 不支持子目录）。
 *
 * 文件名约定：
 *   A:/{agent}_{status}.png  例如 A:/codex_thinking.png, A:/claude_working.png
 *   "codex-cli"/"codex:" 前缀 → "codex"
 *   "claude-code"/"claude" → "claude"
 *   空/未知 agent → "default"
 *   CONNECTING 复用同 agent 的 _thinking.png
 *
 * 缺失/失败全程矢量兜底（pet_ui 内建）。
 */
#include "sdkconfig.h"

#if CONFIG_PETLINK_UI_PNG

#include "pet_assets.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "draw/lv_image_decoder_private.h"

static const char *TAG = "pet_assets";

#define PNG_DISPLAY 280
#define PNG_NATURAL 352
#define PNG_HALF    (PNG_NATURAL / 2)
#define MAX_AGENTS  6

static lv_obj_t *s_img;

/* 预制解码的已知 agent 列表 */
static const char *k_agents[] = { "default", "codex", "claude" };
#define N_AGENTS (sizeof(k_agents) / sizeof(k_agents[0]))

/* has[agent][status]: 0/1 */
static bool s_has[3][5];

static const char *status_name(pl_status_t st)
{
  switch (st) {
    case PL_ST_THINKING:         return "thinking";
    case PL_ST_WORKING:          return "working";
    case PL_ST_WAITING_CONFIRM:  return "waiting_confirm";
    case PL_ST_DONE:             return "done";
    case PL_ST_ERROR:            return "error";
    default:                     return "thinking";
  }
}

static int agent_index(const char *agent)
{
  if (!agent || !agent[0]) return 0;
  if (strncmp(agent, "codex", 5) == 0) return 1;
  if (strcmp(agent, "claude-code") == 0 || strcmp(agent, "claude") == 0) return 2;
  return 0;
}

void pet_assets_init(lv_obj_t *parent)
{
  const esp_vfs_fat_mount_config_t cfg = {
    .max_files = 8,
    .format_if_mount_failed = false,
    .disk_status_check_enable = false,
  };
  esp_err_t err = esp_vfs_fat_spiflash_mount_ro("/asset", "asset", &cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "asset 分区挂载失败（%s）→ 全程矢量兜底", esp_err_to_name(err));
    return;
  }

  memset(s_has, 0, sizeof(s_has));

  /* 扫描所有 (agent, status) 组合 */
  static const pl_status_t states[] = {
    PL_ST_THINKING, PL_ST_WORKING, PL_ST_WAITING_CONFIRM, PL_ST_DONE, PL_ST_ERROR
  };
  int total = 0;
  for (int ai = 0; ai < (int)N_AGENTS; ai++) {
    for (int si = 0; si < 5; si++) {
      char abspath[64];
      snprintf(abspath, sizeof(abspath), "/asset/%s_%s.png",
               k_agents[ai], status_name(states[si]));
      struct stat sb;
      if (stat(abspath, &sb) != 0 || sb.st_size <= 0) continue;

      char lvpath[64];
      snprintf(lvpath, sizeof(lvpath), "A:/%s_%s.png",
               k_agents[ai], status_name(states[si]));
      lv_image_decoder_dsc_t dsc;
      if (lv_image_decoder_open(&dsc, lvpath, NULL) == LV_RESULT_OK) {
        lv_image_decoder_close(&dsc);
        s_has[ai][si] = true;
        total++;
      }
    }
  }

  s_img = lv_image_create(parent);
  lv_obj_set_size(s_img, PNG_DISPLAY, PNG_DISPLAY);
  lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

  ESP_LOGI(TAG, "asset 挂载完成：%d 张 PNG 就绪（%d agent）", total, (int)N_AGENTS);
}

bool pet_assets_has(pl_status_t st)
{
  (void)st;
  for (int ai = 0; ai < (int)N_AGENTS; ai++)
    for (int si = 0; si < 5; si++)
      if (s_has[ai][si]) return true;
  return false;
}

bool pet_assets_show(pl_status_t st, bool on, const char *agent)
{
  if (!s_img) return false;
  if (!on) { lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN); return false; }

  int ai = agent_index(agent);
  int st_idx = -1;
  static const pl_status_t states[] = {
    PL_ST_THINKING, PL_ST_WORKING, PL_ST_WAITING_CONFIRM, PL_ST_DONE, PL_ST_ERROR
  };
  for (int i = 0; i < 5; i++) {
    if (states[i] == st || (st == PL_ST_CONNECTING && states[i] == PL_ST_THINKING)) {
      st_idx = i;
      break;
    }
  }

  if (st_idx < 0 || !s_has[ai][st_idx]) {
    /* CONNECTING 回退 thinking */
    if (st == PL_ST_CONNECTING && s_has[ai][0]) {
      char lvpath[64];
      snprintf(lvpath, sizeof(lvpath), "A:/%s_thinking.png", k_agents[ai]);
      lv_image_set_src(s_img, lvpath);
      lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
      return true;
    }
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    return false;
  }

  char lvpath[64];
  snprintf(lvpath, sizeof(lvpath), "A:/%s_%s.png", k_agents[ai], status_name(st));
  lv_image_set_src(s_img, lvpath);
  lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
  return true;
}

void pet_assets_move(int cx, int cy)
{
  if (s_img) lv_obj_set_pos(s_img, cx - PNG_DISPLAY / 2, cy - PNG_DISPLAY / 2);
}

#endif /* CONFIG_PETLINK_UI_PNG */
