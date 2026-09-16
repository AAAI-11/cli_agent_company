/**
 * 宠物 UI 实现（B3：六态矢量小狗 + 动画，与 virtual_device/pet.js 同源）。
 *
 * 横屏架构：面板物理扫描固定 480 列×640 行（竖）且不可改——LVGL 以 640×480
 * 横向画布布局，flush_cb 逐像素转置进 raster 级冲再 draw_bitmap。模块须
 * 物理旋转 90° 安装（默认顺时针 PET_UI_ROT_CW=1；装反方向=镜像/颠倒，
 * 改 0 重编即可）。
 *
 * 坐标制：横屏逻辑画布 320×240（X2 → 640×480 物理）。宠物各部件常量按
 * pet.js 竖版心智取值（头部 r52、眼位 ±18、动画相位系数 450/160/40 等均与
 * pet.js 逐字对齐），仅画布中心从 (120,150) 挪到 (160,120)。
 * 矢量小狗 = lv_obj 拼装（圆/胶囊/旋转矩形/lv_arc 指示弧），无图片依赖。
 *
 * 动画模型：唯一 ui_tick(33ms) 每拍 (1)取快照 (2)按 lv_tick_get() 推相位
 * （bob/shake/星轨/弧转/脉冲），(3)状态变化才重刷样式族并打 applied 度量日志。
 */
#include "pet_ui.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"

#include "lvgl.h"

#include "ui_map.h"

static const char *TAG = "pet_ui";

#define DISP_W          640                /* 横屏逻辑宽（LVGL 画布） */
#define DISP_H          480
#define RASTER_W        480                /* 面板物理扫描：480 列×640 行，不可改 */
#define RASTER_H        640
#define PET_UI_ROT_CW   1                  /* 模块顺时针转 90° 安装；0=逆时针 */
#define DRAW_BUF_LINES  64                 /* 官方例程取 V_RES/10 */
#define TICK_PERIOD_MS  2
#define UI_TICK_MS      33                 /* ~30fps 的界面节拍 */
#define TASK_STACK      8192
#define TASK_PRIO       4                  /* 低于 petlink(5)：协议优先 */

LV_FONT_DECLARE(font_pet_32);
LV_FONT_DECLARE(font_pet_text_24);

#include "pet_assets.h"

/* pet.js 部件常量（逻辑值）→ 横屏物理值 = X2；画布中心 = 横屏几何中心 */
#define X2(v)          ((v) * 2)
#define PET_CX         X2(160)             /* 宠物中心 (320,240) 物理 */
#define PET_CY         X2(120)

/* 宠物包围盒：LVGL 会把子件裁剪到父容器边界框内（1×1 容器只剩一个点
 * 的真机教训）——容器必须罩住全部子件，子件坐标 = 相对盒左上（恒正） */
#define DOG_BBOX_W     208                 /* 头 r104 ± */
#define DOG_BBOX_H     220                 /* 耳尖 -116 .. 头底 +104 */
#define DOG_OX         104                 /* 盒左上 → 宠物中心 */
#define DOG_OY         116

/* Tokyo Night 调色板（pet.js 抄录） */
#define COL_BG         0x0d0f17
#define COL_HEAD       0xf3f6fc
#define COL_EAR        0xe2e6f3
#define COL_BLUSH      0xf5b6c2
#define COL_INK        0x3b4261
#define COL_SWEAT      0x7dcfff
#define COL_TOPTEXT    0x565f89

/* Agent 标识 → 显示标签 + 颜色 */
#define AGENT_COLOR_CODEX  0x7aa2f7  /* blue */
#define AGENT_COLOR_CLAUDE 0x9ece6a  /* green */
#define AGENT_COLOR_OTHER  0x565f89  /* gray */

/* ---------------- 快照（唯一跨任务通道） ---------------- */

static SemaphoreHandle_t s_lock;
static pet_ui_snap_t s_snap = {
  .state = PL_ST_CONNECTING,
  .link_live = false,
  .session = "",
};

void pet_ui_push(const pet_ui_snap_t *snap)
{
  if (!s_lock || !snap) return;
  pet_ui_snap_t tmp = *snap;
  tmp.arrived_ms = (uint32_t)(esp_timer_get_time() / 1000);
  if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_snap = tmp;
    xSemaphoreGive(s_lock);
  }
}

void pet_ui_link(bool live)
{
  if (!s_lock) return;
  if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_snap.link_live = live;
    xSemaphoreGive(s_lock);
  }
}

/* ---------------- LVGL 底座（照官方例程） ---------------- */

static esp_lcd_panel_handle_t s_panel;

static uint8_t *s_rot;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
  esp_lcd_panel_handle_t p = lv_display_get_user_data(disp);
  const int32_t w = area->x2 - area->x1 + 1;

  int32_t r1, r2, c1, c2;
#if PET_UI_ROT_CW
  r1 = RASTER_H - 1 - area->x2;  r2 = RASTER_H - 1 - area->x1;
  c1 = area->y1;                 c2 = area->y2;
#else
  r1 = area->x1;                 r2 = area->x2;
  c1 = RASTER_W - 1 - area->y2;  c2 = RASTER_W - 1 - area->y1;
#endif

  const int32_t rw = c2 + 1 - c1;
  for (int32_t r = r1; r <= r2; r++) {
    uint8_t *dst = s_rot + (size_t)(r - r1) * rw * 3;
    for (int32_t c = c1; c <= c2; c++) {
#if PET_UI_ROT_CW
      const uint8_t *src = px_map +
          ((size_t)(c - area->y1) * w + (RASTER_H - 1 - r) - area->x1) * 3;
#else
      const uint8_t *src = px_map +
          ((size_t)(RASTER_W - 1 - c) - area->y1) * w * 3 +
          (size_t)(r - area->x1) * 3;
#endif
      dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
      dst += 3;
    }
  }
  esp_lcd_panel_draw_bitmap(p, c1, r1, c2 + 1, r2 + 1, s_rot);
}

static bool notify_flush_ready(esp_lcd_panel_handle_t panel,
                              esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
  (void)panel;
  (void)edata;
  lv_display_flush_ready((lv_display_t *)user_ctx);
  return false;
}

static void increase_lvgl_tick(void *arg)
{
  (void)arg;
  lv_tick_inc(TICK_PERIOD_MS);
}

/* ---------------- 小控件工厂：无边界纯色形状 ---------------- */

static lv_obj_t *shape(lv_obj_t *parent, uint32_t color, int32_t w, int32_t h,
                       int32_t radius)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style(o, NULL, LV_PART_ANY | LV_STATE_ANY);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  return o;
}

static lv_obj_t *blob(lv_obj_t *parent, uint32_t color, int32_t w, int32_t h,
                      int32_t x, int32_t y)
{
  lv_obj_t *o = shape(parent, color, w, h, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(o, x, y);
  return o;
}

static lv_obj_t *rect(lv_obj_t *parent, uint32_t color, int32_t w, int32_t h,
                      int32_t x, int32_t y, int32_t radius)
{
  lv_obj_t *o = shape(parent, color, w, h, radius);
  lv_obj_set_pos(o, x, y);
  return o;
}

static lv_obj_t *deco_arc(lv_obj_t *parent, int32_t w, int32_t x, int32_t y,
                          int32_t a0, int32_t a1, int32_t width, uint32_t color)
{
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_remove_style(a, NULL, LV_PART_ANY | LV_STATE_ANY);
  lv_obj_set_size(a, w, w);
  lv_obj_set_pos(a, x, y);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  lv_arc_set_mode(a, LV_ARC_MODE_NORMAL);
  lv_arc_set_bg_angles(a, a0, a1);
  lv_arc_set_range(a, 0, 100);
  lv_arc_set_value(a, 100);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  return a;
}

/* ---------------- 控件句柄 ---------------- */

static lv_obj_t *s_topbar;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_session;
static lv_obj_t *s_lbl_agent;            /* agent 标识徽标 */
static lv_obj_t *s_bubble[PL_MAX_BUBBLES];
static lv_obj_t *s_lbl_tokens;
static lv_obj_t *s_halo;
static lv_obj_t *s_dog;
static lv_obj_t *s_lbl_state;
static lv_obj_t *s_text;

/* 五套表情 */
static lv_obj_t *s_eye_think[2], *s_mouth_think;
static lv_obj_t *s_eye_wait[2],  *s_mouth_wait;
static lv_obj_t *s_eye_work[2],  *s_brow_work[2], *s_mouth_work;
static lv_obj_t *s_eye_done[2],  *s_mouth_done;
static lv_obj_t *s_eye_err[4],   *s_mouth_err[3], *s_sweat;

/* 各态附件 */
static lv_obj_t *s_dots[3];
static lv_obj_t *s_arc_work[2];
static lv_obj_t *s_bang;
static lv_obj_t *s_stars[5];

/** agent 标识 → 颜色 */
static uint32_t agent_color(const char *agent) {
  if (!agent || !agent[0]) return AGENT_COLOR_OTHER;
  if (strncmp(agent, "codex", 5) == 0) return AGENT_COLOR_CODEX;
  if (strcmp(agent, "claude-code") == 0 || strcmp(agent, "claude") == 0) return AGENT_COLOR_CLAUDE;
  return AGENT_COLOR_OTHER;
}

/** agent 标识 → 短标签 */
static const char *agent_label(const char *agent) {
  if (!agent || !agent[0]) return "";
  if (strncmp(agent, "codex", 5) == 0) return "CX";
  if (strcmp(agent, "claude-code") == 0 || strcmp(agent, "claude") == 0) return "CC";
  return "";
}

/* ---------------- 页面搭建 ---------------- */

static void build_page(lv_display_t *disp)
{
  (void)disp;
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);

  s_topbar = shape(scr, 0xffffff, DISP_W, X2(4), 0);
  lv_obj_set_pos(s_topbar, 0, 0);

  s_lbl_title = lv_label_create(scr);
  lv_label_set_text(s_lbl_title, "PETLINK/1");
  lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(COL_TOPTEXT), 0);
  lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_20, 0);
  lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 12, 10);

  /* Agent 标识：标题右侧小色块 + 标签 */
  s_lbl_agent = lv_label_create(scr);
  lv_label_set_text(s_lbl_agent, "");
  lv_obj_set_style_text_font(s_lbl_agent, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_agent, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(s_lbl_agent, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(s_lbl_agent, 6, 0);
  lv_obj_set_style_pad_ver(s_lbl_agent, 2, 0);
  lv_obj_set_style_radius(s_lbl_agent, 4, 0);
  lv_obj_align(s_lbl_agent, LV_ALIGN_TOP_LEFT, 140, 12);
  lv_obj_add_flag(s_lbl_agent, LV_OBJ_FLAG_HIDDEN);

  s_lbl_session = lv_label_create(scr);
  lv_label_set_text(s_lbl_session, "# --");
  lv_obj_set_style_text_color(s_lbl_session, lv_color_hex(COL_TOPTEXT), 0);
  lv_obj_set_style_text_font(s_lbl_session, &lv_font_montserrat_16, 0);
  lv_obj_align(s_lbl_session, LV_ALIGN_TOP_RIGHT, -12, 14);

  for (int i = 0; i < PL_MAX_BUBBLES; i++) {
    s_bubble[i] = blob(scr, 0xffffff, 20, 20, -40, -40);
    lv_obj_add_flag(s_bubble[i], LV_OBJ_FLAG_HIDDEN);
  }

  s_lbl_tokens = lv_label_create(scr);
  lv_label_set_text(s_lbl_tokens, "");
  lv_obj_set_style_text_color(s_lbl_tokens, lv_color_hex(COL_TOPTEXT), 0);
  lv_obj_set_style_text_font(s_lbl_tokens, &lv_font_montserrat_14, 0);
  lv_obj_align(s_lbl_tokens, LV_ALIGN_TOP_LEFT, 12, 44);
  lv_obj_add_flag(s_lbl_tokens, LV_OBJ_FLAG_HIDDEN);

  s_halo = blob(scr, 0xffffff, X2(130) * 2, X2(130) * 2,
                PET_CX - X2(130), PET_CY - X2(130));
  lv_obj_set_style_bg_opa(s_halo, LV_OPA_20, 0);

  pet_assets_init(scr);

  s_dog = shape(scr, 0, DOG_BBOX_W, DOG_BBOX_H, 0);
  lv_obj_set_style_bg_opa(s_dog, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(s_dog, PET_CX - DOG_OX, PET_CY - DOG_OY);

  lv_obj_t *ear_l = blob(s_dog, COL_EAR, X2(13), X2(26), DOG_OX - X2(37) - X2(13),
                         DOG_OY - X2(29) - X2(26));
  lv_obj_set_style_transform_rotation(ear_l, -286, 0);
  lv_obj_t *ear_r = blob(s_dog, COL_EAR, X2(13), X2(26), DOG_OX + X2(37) - X2(13),
                         DOG_OY - X2(29) - X2(26));
  lv_obj_set_style_transform_rotation(ear_r, 286, 0);

  blob(s_dog, COL_HEAD, X2(52) * 2, X2(52) * 2, DOG_OX - X2(52),
       DOG_OY - X2(52));
  blob(s_dog, COL_BLUSH, X2(9) * 2, X2(6) * 2, DOG_OX - X2(28) - X2(9),
       DOG_OY + X2(14) - X2(6));
  blob(s_dog, COL_BLUSH, X2(9) * 2, X2(6) * 2, DOG_OX + X2(28) - X2(9),
       DOG_OY + X2(14) - X2(6));

  s_eye_think[0] = blob(s_dog, COL_INK, X2(9), X2(9), DOG_OX - X2(18) - X2(4),
                        DOG_OY - X2(12) - X2(4));
  s_eye_think[1] = blob(s_dog, COL_INK, X2(9), X2(9), DOG_OX + X2(18) - X2(4),
                        DOG_OY - X2(12) - X2(4));
  s_mouth_think = deco_arc(s_dog, X2(5) * 2, DOG_OX - X2(5),
                           DOG_OY + X2(10) - X2(5), 18, 162, 7, COL_INK);

  s_eye_wait[0] = blob(s_dog, COL_INK, X2(9), X2(9), DOG_OX - X2(18) - X2(4),
                       DOG_OY - X2(8) - X2(4));
  s_eye_wait[1] = blob(s_dog, COL_INK, X2(9), X2(9), DOG_OX + X2(18) - X2(4),
                       DOG_OY - X2(8) - X2(4));
  s_mouth_wait = deco_arc(s_dog, X2(6) * 2, DOG_OX - X2(6),
                          DOG_OY + X2(10) - X2(6), 0, 360, 7, COL_INK);

  s_brow_work[0] = rect(s_dog, COL_INK, 30, 6, DOG_OX - X2(18) - 15,
                        DOG_OY - X2(12) - 3, 3);
  lv_obj_set_style_transform_rotation(s_brow_work[0], 159, 0);
  s_brow_work[1] = rect(s_dog, COL_INK, 30, 6, DOG_OX + X2(18) - 15,
                        DOG_OY - X2(12) - 3, 3);
  lv_obj_set_style_transform_rotation(s_brow_work[1], -159, 0);
  s_eye_work[0] = blob(s_dog, COL_INK, X2(9), X2(9), DOG_OX - X2(18) - X2(4),
                       DOG_OY - X2(6) - X2(4));
  s_eye_work[1] = blob(s_dog, COL_INK, X2(9), X2(9), DOG_OX + X2(18) - X2(4),
                       DOG_OY - X2(6) - X2(4));
  s_mouth_work = rect(s_dog, COL_INK, X2(7) * 2, 7, DOG_OX - X2(7),
                      DOG_OY + X2(12) - 3, 3);

  s_eye_done[0] = deco_arc(s_dog, X2(6) * 2, DOG_OX - X2(18) - X2(6),
                           DOG_OY - X2(5) - X2(6), 198, 342, 7, COL_INK);
  s_eye_done[1] = deco_arc(s_dog, X2(6) * 2, DOG_OX + X2(18) - X2(6),
                           DOG_OY - X2(5) - X2(6), 198, 342, 7, COL_INK);
  s_mouth_done = deco_arc(s_dog, X2(10) * 2, DOG_OX - X2(10),
                          DOG_OY + X2(8) - X2(10), 27, 153, 7, COL_INK);

  for (int i = 0; i < 2; i++) {
    int32_t cx = DOG_OX + ((i == 0) ? -X2(18) : X2(18));
    s_eye_err[i * 2] = rect(s_dog, COL_INK, 22, 6, cx - 11,
                            DOG_OY - X2(8) - 3, 3);
    lv_obj_set_style_transform_rotation(s_eye_err[i * 2], 450, 0);
    s_eye_err[i * 2 + 1] = rect(s_dog, COL_INK, 22, 6, cx - 11,
                                DOG_OY - X2(8) - 3, 3);
    lv_obj_set_style_transform_rotation(s_eye_err[i * 2 + 1], -450, 0);
  }
  for (int i = 0; i < 3; i++) {
    s_mouth_err[i] = rect(s_dog, COL_INK, 12, 6, DOG_OX - X2(9) + i * X2(9) - 6,
                          DOG_OY + X2(12) - 3, 3);
    lv_obj_set_style_transform_rotation(s_mouth_err[i], (i % 2 ? -450 : 450), 0);
  }
  s_sweat = blob(s_dog, COL_SWEAT, 12, 20, DOG_OX + X2(38), DOG_OY - X2(30));

  s_lbl_state = lv_label_create(scr);
  lv_label_set_text(s_lbl_state, ui_style_for(PL_ST_CONNECTING)->label);
  lv_obj_set_style_text_font(s_lbl_state, &font_pet_32, 0);
  lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_lbl_state, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_lbl_state,
                            lv_color_hex(ui_style_for(PL_ST_CONNECTING)->fg), 0);
  lv_obj_set_style_radius(s_lbl_state, 14, 0);
  lv_obj_set_style_pad_hor(s_lbl_state, 16, 0);
  lv_obj_set_style_pad_ver(s_lbl_state, 6, 0);
  lv_obj_align(s_lbl_state, LV_ALIGN_BOTTOM_MID, 0, -56);

  s_text = lv_label_create(scr);
  lv_obj_set_style_text_font(s_text, &font_pet_text_24, 0);
  lv_obj_set_style_text_color(s_text, lv_color_hex(0x9aa5ce), 0);
  {
    char line[64];
    ui_text_line(NULL, PL_ST_CONNECTING, line, sizeof(line));
    lv_label_set_text(s_text, line);
  }
  lv_obj_align(s_text, LV_ALIGN_BOTTOM_MID, 0, -18);

  for (int i = 0; i < 3; i++) {
    s_dots[i] = blob(scr, 0xffffff, X2(4) * 2, X2(4) * 2,
                     PET_CX - X2(14) + i * X2(14) - X2(4), PET_CY - X2(78) - X2(4));
  }
  s_arc_work[0] = deco_arc(scr, X2(84) * 2, PET_CX - X2(84), PET_CY - X2(84),
                           0, 63, 8, 0xffffff);
  s_arc_work[1] = deco_arc(scr, X2(84) * 2, PET_CX - X2(84), PET_CY - X2(84),
                           180, 243, 8, 0xffffff);
  s_bang = blob(scr, 0xffffff, X2(13) * 2, X2(13) * 2,
                PET_CX + X2(60) - X2(13), PET_CY - X2(66) - X2(13));
  lv_obj_set_style_transform_pivot_x(s_bang, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(s_bang, lv_pct(50), 0);
  lv_obj_t *bang_txt = lv_label_create(s_bang);
  lv_label_set_text(bang_txt, "！");
  lv_obj_set_style_text_font(bang_txt, &font_pet_32, 0);
  lv_obj_set_style_text_color(bang_txt, lv_color_hex(COL_BG), 0);
  lv_obj_center(bang_txt);
  for (int i = 0; i < 5; i++) {
    s_stars[i] = lv_label_create(scr);
    lv_label_set_text(s_stars[i], "★");
    lv_obj_set_style_text_font(s_stars[i], &font_pet_32, 0);
  }
}

/* ---------------- 状态应用 ---------------- */

static void set_vis(lv_obj_t *o, bool on)
{
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void apply_state(pl_status_t st, const pet_ui_snap_t *snap)
{
  const ui_style_t *sty = ui_style_for(st);
  uint32_t fg = sty->fg;
  lv_color_t c = lv_color_hex(fg);

  lv_obj_set_style_bg_color(s_topbar, c, 0);
  lv_obj_set_style_bg_color(s_halo, c, 0);
  lv_obj_set_style_bg_color(s_lbl_state, c, 0);
  lv_label_set_text(s_lbl_state, sty->label);

  /* 素材按 agent 选目录；只有该 agent 有对应 PNG 才替换矢量狗 */
  bool use_png = pet_assets_has(st);
  pet_assets_show(st, use_png, snap->agent);
  set_vis(s_dog, !use_png);
  for (int i = 0; i < 2; i++)
    lv_obj_set_style_arc_color(s_arc_work[i], c, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bang, c, 0);
  for (int i = 0; i < 3; i++)
    lv_obj_set_style_bg_color(s_dots[i], c, 0);
  for (int i = 0; i < 5; i++)
    lv_obj_set_style_text_color(s_stars[i], c, 0);

  bool is_think = (st == PL_ST_THINKING || st == PL_ST_CONNECTING);
  for (int i = 0; i < 2; i++) {
    set_vis(s_eye_think[i], is_think);
    set_vis(s_eye_wait[i], st == PL_ST_WAITING_CONFIRM);
    set_vis(s_eye_work[i], st == PL_ST_WORKING);
    set_vis(s_eye_done[i], st == PL_ST_DONE);
    set_vis(s_brow_work[i], st == PL_ST_WORKING);
  }
  set_vis(s_mouth_think, is_think);
  set_vis(s_mouth_wait, st == PL_ST_WAITING_CONFIRM);
  set_vis(s_mouth_work, st == PL_ST_WORKING);
  set_vis(s_mouth_done, st == PL_ST_DONE);
  for (int i = 0; i < 4; i++) set_vis(s_eye_err[i], st == PL_ST_ERROR);
  for (int i = 0; i < 3; i++) set_vis(s_mouth_err[i], st == PL_ST_ERROR);
  set_vis(s_sweat, st == PL_ST_ERROR);

  for (int i = 0; i < 3; i++) set_vis(s_dots[i], is_think && i == 0);
  for (int i = 0; i < 2; i++) set_vis(s_arc_work[i], st == PL_ST_WORKING);
  set_vis(s_bang, st == PL_ST_WAITING_CONFIRM);
  for (int i = 0; i < 5; i++) set_vis(s_stars[i], st == PL_ST_DONE);

  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  ESP_LOGI(TAG, "pet_ui: state=%s agent=%s applied=%ums", pl_status_name(st),
           snap->agent[0] ? snap->agent : "?",
           now_ms - snap->arrived_ms);
}

static void apply_bubbles(const pet_ui_snap_t *snap)
{
  uint8_t sig[PL_MAX_BUBBLES + 1];
  sig[0] = snap->bubble_n;
  for (int i = 0; i < PL_MAX_BUBBLES; i++) {
    sig[i + 1] = (i < snap->bubble_n)
        ? (uint8_t)(snap->bubbles[i].st * 2 + (snap->bubbles[i].focus ? 1 : 0))
        : 0xFF;
  }
  static uint8_t last_sig[PL_MAX_BUBBLES + 1];
  if (memcmp(sig, last_sig, sizeof(sig)) == 0) return;
  memcpy(last_sig, sig, sizeof(sig));

  const int n = snap->bubble_n;
  for (int i = 0; i < PL_MAX_BUBBLES; i++) {
    lv_obj_t *o = s_bubble[i];
    if (i >= n) {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    const bool foc = snap->bubbles[i].focus;
    const int32_t size = foc ? 28 : 20;
    const int32_t cx = DISP_W - 12 - 17 - (n - 1 - i) * 36;
    const int32_t cy = 52;
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, cx - size / 2, cy - size / 2);
    lv_obj_set_style_bg_color(o, lv_color_hex(ui_style_for(snap->bubbles[i].st)->fg), 0);
    if (foc) {
      lv_obj_set_style_border_width(o, 2, 0);
      lv_obj_set_style_border_color(o, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_border_width(o, 0, 0);
    }
  }
  int fi = -1;
  for (int i = 0; i < n; i++) if (snap->bubbles[i].focus) { fi = i; break; }
  ESP_LOGI(TAG, "pet_ui: bubbles=%d focus@%d(%s)", n, fi,
           fi >= 0 ? pl_status_name(snap->bubbles[fi].st) : "-");
}

static void fmt_tok(char *out, size_t n, uint32_t v)
{
  if (v >= 1000000u) {
    snprintf(out, n, "%u.%uM", (unsigned)(v / 1000000u), (unsigned)((v % 1000000u) / 100000u));
  } else if (v >= 1000u) {
    snprintf(out, n, "%u.%uk", (unsigned)(v / 1000u), (unsigned)((v % 1000u) / 100u));
  } else {
    snprintf(out, n, "%u", (unsigned)v);
  }
}

static void apply_tokens(const pet_ui_snap_t *snap)
{
  struct sig_t { uint8_t valid; uint32_t ctx; uint32_t tot; } sig;
  sig.valid = snap->tokens_valid ? 1 : 0;
  sig.ctx = snap->tokens_valid ? snap->tokens.ctx : 0;
  sig.tot = snap->tokens_valid ? snap->tokens.tot : 0;
  static struct sig_t last;
  if (memcmp(&sig, &last, sizeof(sig)) == 0) return;
  last = sig;

  if (!sig.valid) {
    lv_obj_add_flag(s_lbl_tokens, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char a[12], b[12], line[40];
  fmt_tok(a, sizeof(a), sig.ctx);
  fmt_tok(b, sizeof(b), sig.tot);
  snprintf(line, sizeof(line), "ctx %s / tot %s", a, b);
  lv_label_set_text(s_lbl_tokens, line);
  lv_obj_clear_flag(s_lbl_tokens, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- 每拍动画 ---------------- */

static void anim_step(pl_status_t st, uint32_t t)
{
  float ft = (float)t;

  float bob = sinf(ft / 450.f) * 4.f, shake = 0.f;
  if (st == PL_ST_WAITING_CONFIRM) bob = fabsf(sinf(ft / 160.f)) * -14.f;
  if (st == PL_ST_ERROR) shake = sinf(ft / 40.f) * 3.f;
  lv_obj_set_pos(s_dog, PET_CX + (int32_t)(shake * 2) - DOG_OX,
                 PET_CY + (int32_t)(bob * 2) - DOG_OY);
  pet_assets_move(PET_CX + (int32_t)(shake * 2), PET_CY + (int32_t)(bob * 2));

  if (st == PL_ST_CONNECTING) {
    int32_t a = 120 + (int32_t)(sinf(ft / 450.f) * 90.f);
    if (a < 40) a = 40;
    lv_obj_set_style_bg_opa(s_topbar, (lv_opa_t)a, 0);
    lv_obj_set_style_bg_opa(s_lbl_state, (lv_opa_t)a, 0);
  } else {
    lv_obj_set_style_bg_opa(s_topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_lbl_state, LV_OPA_COVER, 0);
  }

  if (st == PL_ST_THINKING || st == PL_ST_CONNECTING) {
    int n = 1 + (int)(t / 350) % 3;
    for (int i = 0; i < 3; i++) set_vis(s_dots[i], i < n);
  }

  if (st == PL_ST_WORKING) {
    int32_t deg = (int32_t)(ft / 300.f * 57.29578f);
    lv_arc_set_bg_angles(s_arc_work[0], deg, deg + 63);
    lv_arc_set_bg_angles(s_arc_work[1], deg + 180, deg + 243);
  }

  if (st == PL_ST_WAITING_CONFIRM) {
    int32_t scale = (int32_t)(256.f * (1.f + 0.15f * sinf(ft / 160.f)));
    lv_obj_set_style_transform_scale(s_bang, scale, 0);
  }

  if (st == PL_ST_DONE) {
    for (int i = 0; i < 5; i++) {
      float a = ft / 500.f + i * 1.25664f;
      float r = (86.f + sinf(ft / 260.f + i) * 6.f) * 2.f;
      lv_obj_set_pos(s_stars[i], PET_CX + (int32_t)(cosf(a) * r) - 16,
                     PET_CY + (int32_t)(sinf(a) * r) - 16);
    }
  }

  if (st == PL_ST_ERROR) {
    lv_obj_set_y(s_sweat, DOG_OY - X2(30) + (int32_t)(sinf(ft / 300.f) * 6.f));
  }
}

/** 刷新 agent 徽标 */
static void apply_agent(const pet_ui_snap_t *snap)
{
  static char last_agent[PL_AGENT_LEN] = "";
  if (strcmp(snap->agent, last_agent) == 0) return;
  snprintf(last_agent, sizeof(last_agent), "%s", snap->agent);

  const char *label = agent_label(snap->agent);
  if (!label[0]) {
    lv_obj_add_flag(s_lbl_agent, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(s_lbl_agent, label);
  lv_obj_set_style_bg_color(s_lbl_agent, lv_color_hex(agent_color(snap->agent)), 0);
  lv_obj_clear_flag(s_lbl_agent, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- 33ms 界面节拍 ---------------- */

static void ui_tick(lv_timer_t *tmr)
{
  (void)tmr;
  pet_ui_snap_t snap;
  if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
  snap = s_snap;
  xSemaphoreGive(s_lock);

  uint32_t now = lv_tick_get();

#if CONFIG_PETLINK_UI_SELFTEST
  static pl_status_t test_st = PL_ST_CONNECTING;
  static uint32_t t_rot = 0;
  if (t_rot == 0 || now - t_rot >= 2000) {
    t_rot = now ? now : 1;
    test_st = (pl_status_t)((test_st + 1) % (PL_ST_ERROR + 1));
  }
  snap.state = test_st;
  snprintf(snap.text, sizeof(snap.text), "%s", ui_style_for(test_st)->sample_text);
#endif

  anim_step(snap.state, now);

  static pl_status_t last = (pl_status_t)-1;
  static char last_sess[16] = "";
  if (snap.state != last) {
    apply_state(snap.state, &snap);
    last = snap.state;
  }
  if (strcmp(snap.session, last_sess) != 0) {
    char top[16];
    snprintf(top, sizeof(top), "# %.10s", snap.session[0] ? snap.session : "--");
    lv_label_set_text(s_lbl_session, top);
    snprintf(last_sess, sizeof(last_sess), "%s", snap.session);
  }
  static char last_text[100] = "";
  static pl_status_t last_text_st = (pl_status_t)-1;
  if (strcmp(snap.text, last_text) != 0 || snap.state != last_text_st) {
    char line[64];
    ui_text_line(snap.text, snap.state, line, sizeof(line));
    lv_label_set_text(s_text, line);
    snprintf(last_text, sizeof(last_text), "%s", snap.text);
    last_text_st = snap.state;
  }

  apply_agent(&snap);
  apply_bubbles(&snap);
  apply_tokens(&snap);
}

/* ---------------- LVGL 任务 ---------------- */

static void lvgl_task(void *arg)
{
  (void)arg;
  ESP_LOGI(TAG, "LVGL 任务启动");
  uint32_t time_till_next_ms = 0;
  while (1) {
    time_till_next_ms = lv_timer_handler();
    if (time_till_next_ms < 10) time_till_next_ms = 10;
    if (time_till_next_ms > 500) time_till_next_ms = 500;
    vTaskDelay(pdMS_TO_TICKS(time_till_next_ms));
  }
}

void pet_ui_mount(esp_lcd_panel_handle_t panel)
{
  s_panel = panel;
  s_lock = xSemaphoreCreateMutex();
  assert(s_lock);

  lv_init();

  lv_display_t *disp = lv_display_create(DISP_W, DISP_H);
  lv_display_set_user_data(disp, panel);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);

  const size_t buf_sz = DISP_W * DRAW_BUF_LINES * sizeof(lv_color_t);
  void *buf1 = heap_caps_calloc(1, buf_sz, MALLOC_CAP_SPIRAM);
  void *buf2 = heap_caps_calloc(1, buf_sz, MALLOC_CAP_SPIRAM);
  assert(buf1 && buf2);
  lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  s_rot = heap_caps_calloc(1, (size_t)RASTER_W * RASTER_H * 3, MALLOC_CAP_SPIRAM);
  assert(s_rot);

  esp_lcd_dpi_panel_event_callbacks_t cbs = {
    .on_color_trans_done = notify_flush_ready,
  };
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, disp));

  const esp_timer_create_args_t tick_args = {
    .callback = &increase_lvgl_tick,
    .name = "lvgl_tick",
  };
  esp_timer_handle_t tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, TICK_PERIOD_MS * 1000));

  build_page(disp);
  apply_state(PL_ST_CONNECTING, &s_snap);
  lv_timer_create(ui_tick, UI_TICK_MS, NULL);

  ESP_LOGI(TAG, "pet_ui 挂载完成（%dx%d 横屏 → %dx%d raster 转置 flush, "
           "draw buf %d 行×2, agent 徽标就位）", DISP_W, DISP_H, RASTER_W, RASTER_H,
           DRAW_BUF_LINES);
  xTaskCreate(lvgl_task, "lvgl", TASK_STACK, NULL, TASK_PRIO, NULL);
}
