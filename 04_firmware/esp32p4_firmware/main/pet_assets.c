/**
 * 五态 PNG 素材槽实现（见 pet_assets.h 顶部约定）。
 *
 * 内存口径：LV_CACHE_DEF_SIZE 开了 8 条目，lodepng 解码结果（352×352
 * ARGB8888 ≈ 494KB/张）经 lv_malloc→malloc 落 PSRAM（sdkconfig 已开
 * SPIRAM_USE_MALLOC，≥16KB 大块自动外置）。
 *
 * 解码时机的坑（2026-09-04 度量实测）：lv_image 文件源的解码发生在渲染期
 * （lv_draw_image → lv_image_decoder_open），单张 lodepng 解码会阻塞
 * LVGL 主循环 ~500ms——状态切换撞上首显解码就贴死宪法 500ms 预算。
 * 故挂载后立即预解码全部在位 PNG 沉淀进 LVGL 缓存（开机多花 1~2s，
 * boot 不在预算内），此后任何态切换都命中缓存（实测 applied ≤33ms）。
 *
 * 预解码成立依赖 sdkconfig 三件套（缺一则静默回退矢量，无任何报错）：
 * LV_USE_CLIB_MALLOC=y（内置 TLSF 池 64KB 装不下 494KB 解码缓冲）；
 * LV_CACHE_DEF_SIZE 按字节扣减（3MB 容 5 张）；
 * stat 必须走 VFS 绝对路径（见下方挂载段注释）。
 */
#include "sdkconfig.h"

#if CONFIG_PETLINK_UI_PNG

#include "pet_assets.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

/* lv_image_decoder_open/close + dsc 完整定义。公开头只有前向声明，
 * 完整结构在私有头（LVGL 自家 src/draw/lv_draw_image.c 同款引入；
 * 本工程 managed_components 锁定 lvgl 版本，升级时需复核）。 */
#include "draw/lv_image_decoder_private.h"

static const char *TAG = "pet_assets";

#define PNG_DISPLAY 176 /* 显示尺寸：352 素材的 2 倍图（浏览器 IMG_SIZE 同口径） */

/* 状态 → 素材路径（LVGL FS 盘符 A = /asset；CONNECTING 复用 thinking） */
static const char *const k_png[] = {
  [PL_ST_CONNECTING]      = "A:/thinking.png",
  [PL_ST_THINKING]        = "A:/thinking.png",
  [PL_ST_WORKING]         = "A:/working.png",
  [PL_ST_WAITING_CONFIRM] = "A:/waiting_confirm.png",
  [PL_ST_DONE]            = "A:/done.png",
  [PL_ST_ERROR]           = "A:/error.png",
};

static lv_obj_t *s_img;
static bool s_has[sizeof(k_png) / sizeof(k_png[0])];

void pet_assets_init(lv_obj_t *parent)
{
  const esp_vfs_fat_mount_config_t cfg = {
    .max_files = 8,
    .format_if_mount_failed = false,
    .disk_status_check_enable = false,
  };
  esp_err_t err = esp_vfs_fat_spiflash_mount_ro("/asset", "asset", &cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "asset 分区挂载失败（%s）→ 全程矢量兜底",
             esp_err_to_name(err));
    return;
  }

  int n = 0;
  for (int st = 0; st < (int)(sizeof(k_png) / sizeof(k_png[0])); st++) {
    struct stat sb;
    /* stat 必须走 IDF VFS 绝对路径（VFS 按注册前缀 "/asset" 匹配，
     * 相对路径匹配不到任何前缀、恒 ENOENT）；"A:/x.png" 仅供 LVGL 用。
     * k_png[CONNECTING] 与 [THINKING] 同名，stat 一次即可，无额外开销。 */
    char abspath[48];
    snprintf(abspath, sizeof abspath, "/asset/%s", k_png[st] + 3);
    s_has[st] = stat(abspath, &sb) == 0 && sb.st_size > 0;
    if (s_has[st]) n++;
  }

  /* 预解码沉淀 LVGL 缓存（见文件头注释）；open 成功即入 cache，
   * close 只减引用不释放缓存条目。解码失败该态自动走矢量兜底。 */
  int precached = 0;
  for (int st = 0; st < (int)(sizeof(k_png) / sizeof(k_png[0])); st++) {
    if (!s_has[st] || (st > 0 && strcmp(k_png[st], k_png[st - 1]) == 0)) continue;
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, k_png[st], NULL) == LV_RESULT_OK) {
      lv_image_decoder_close(&dsc);
      precached++;
    } else {
      ESP_LOGW(TAG, "预解码失败（%s）→ 该态走矢量", k_png[st]);
      s_has[st] = false;
    }
  }

  s_img = lv_image_create(parent);
  lv_image_set_scale(s_img, 256 * PNG_DISPLAY / 352);
  lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
  /* 初始位置在屏外；pet_ui 的 anim_step 每拍 pet_assets_move 校正 */

  ESP_LOGI(TAG, "asset 挂载完成：PNG %d/%d 张就位，预解码 %d 张入缓存（缺/败的态走矢量）",
           n, (int)(sizeof(k_png) / sizeof(k_png[0])), precached);
}

bool pet_assets_has(pl_status_t st)
{
  int i = (int)st;
  return i >= 0 && i < (int)(sizeof(s_has) / sizeof(s_has[0])) && s_has[i];
}

void pet_assets_show(pl_status_t st, bool on)
{
  if (!s_img) return;
  if (on && pet_assets_has(st)) {
    lv_image_set_src(s_img, k_png[st]);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
  }
}

void pet_assets_move(int cx, int cy)
{
  if (s_img) lv_obj_set_pos(s_img, cx - PNG_DISPLAY, cy - PNG_DISPLAY);
}

#endif /* CONFIG_PETLINK_UI_PNG */
