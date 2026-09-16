/**
 * 五态 PNG 素材槽实现——支持按 agent 分目录。
 *
 * 约定：
 *   A:/<agent>/<status>.png  例如 A:/codex/thinking.png, A:/claude/working.png
 *   "codex-cli" 或 "codex:" 前缀 → 目录 "codex"
 *   "claude-code" 或 "claude" → 目录 "claude"
 *   空/未知 agent → 目录 "default"
 *   CONNECTING 复用同目录下的 thinking.png
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

#define PNG_DISPLAY 176

/* 每 agent 目录最多 5 张图（thinking/working/waiting_confirm/done/error）；
 * 当前最多跟踪 3 个 agent（默认 + codex + claude），暂不动态扩容 */
#define MAX_AGENT_DIRS 6
#define STATES_PER_DIR 5

static const pl_status_t k_status_order[STATES_PER_DIR] = {
  PL_ST_THINKING, PL_ST_WORKING, PL_ST_WAITING_CONFIRM, PL_ST_DONE, PL_ST_ERROR,
};

static lv_obj_t *s_img;
static bool s_has[MAX_AGENT_DIRS][STATES_PER_DIR]; /* has[dirIdx][stIdx] */
static char s_agents[MAX_AGENT_DIRS][16];           /* 目录名 */
static int s_dir_n;                                 /* 实际 agent 目录数 */

/** agent 标识 → 目录名，空/未知 → "default" */
static const char *agent_dir(const char *agent)
{
  if (!agent || !agent[0]) return "default";
  if (strncmp(agent, "codex", 5) == 0) return "codex";
  if (strcmp(agent, "claude-code") == 0 || strcmp(agent, "claude") == 0) return "claude";
  return "default";
}

/** 查找或注册一个 agent 目录索引 */
static int dir_idx(const char *agent)
{
  const char *dir = agent_dir(agent);
  for (int i = 0; i < s_dir_n; i++) {
    if (strcmp(s_agents[i], dir) == 0) return i;
  }
  if (s_dir_n >= MAX_AGENT_DIRS) return -1;
  snprintf(s_agents[s_dir_n], sizeof(s_agents[s_dir_n]), "%s", dir);
  return s_dir_n++;
}

/** 检查并预解码指定 agent 目录的全部 PNG */
static void scan_agent_dir(int di)
{
  for (int si = 0; si < STATES_PER_DIR; si++) {
    s_has[di][si] = false;
    pl_status_t st = k_status_order[si];
    const char *name = pl_status_name(st);
    char abspath[64];
    snprintf(abspath, sizeof(abspath), "/asset/%s/%s.png", s_agents[di], name);
    struct stat sb;
    if (stat(abspath, &sb) != 0 || sb.st_size <= 0) continue;

    char lvpath[64];
    snprintf(lvpath, sizeof(lvpath), "A:/%s/%s.png", s_agents[di], name);
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, lvpath, NULL) == LV_RESULT_OK) {
      lv_image_decoder_close(&dsc);
      s_has[di][si] = true;
    }
  }
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

  s_dir_n = 0;
  memset(s_has, 0, sizeof(s_has));

  /* 探测已知目录 */
  static const char *known[] = { "default", "codex", "claude" };
  for (int k = 0; k < (int)(sizeof(known) / sizeof(known[0])); k++) {
    char d[64];
    snprintf(d, sizeof(d), "/asset/%s", known[k]);
    struct stat sb;
    if (stat(d, &sb) == 0 && S_ISDIR(sb.st_mode)) {
      int di = dir_idx(known[k]);
      if (di >= 0) scan_agent_dir(di);
    }
  }

  s_img = lv_image_create(parent);
  lv_image_set_scale(s_img, 256 * PNG_DISPLAY / 352);
  lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

  int total = 0;
  for (int di = 0; di < s_dir_n; di++)
    for (int si = 0; si < STATES_PER_DIR; si++)
      if (s_has[di][si]) total++;
  ESP_LOGI(TAG, "asset 挂载完成：%d agent 目录 %d 张 PNG 就绪", s_dir_n, total);
}

bool pet_assets_has(pl_status_t st)
{
  (void)st;
  return true; /* 具体 agent 看在 pet_assets_show 里，这里仅作为"该态可能有图"的乐观回答 */
}

void pet_assets_show(pl_status_t st, bool on, const char *agent)
{
  if (!s_img) return;
  if (!on) { lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN); return; }

  /* 确定 agent 目录 */
  int di = dir_idx(agent);
  if (di < 0) { lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN); return; }

  /* 状态 → 状态索引 */
  int si = -1;
  for (int i = 0; i < STATES_PER_DIR; i++) {
    if (k_status_order[i] == st || (st == PL_ST_CONNECTING && k_status_order[i] == PL_ST_THINKING)) {
      si = i;
      break;
    }
  }
  if (si < 0 || !s_has[di][si]) {
    /* CONNECTING 或没有对应图：回退到 thinking */
    if (st == PL_ST_CONNECTING) {
      int think_si = -1;
      for (int i = 0; i < STATES_PER_DIR; i++) {
        if (k_status_order[i] == PL_ST_THINKING) { think_si = i; break; }
      }
      if (think_si >= 0 && s_has[di][think_si]) {
        char lvpath[64];
        snprintf(lvpath, sizeof(lvpath), "A:/%s/thinking.png", s_agents[di]);
        lv_image_set_src(s_img, lvpath);
        lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        return;
      }
    }
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  char lvpath[64];
  snprintf(lvpath, sizeof(lvpath), "A:/%s/%s.png", s_agents[di], pl_status_name(st));
  lv_image_set_src(s_img, lvpath);
  lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
}

void pet_assets_move(int cx, int cy)
{
  if (s_img) lv_obj_set_pos(s_img, cx - PNG_DISPLAY, cy - PNG_DISPLAY);
}

#endif /* CONFIG_PETLINK_UI_PNG */
