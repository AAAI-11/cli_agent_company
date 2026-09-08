/**
 * OLED 渲染层（纯 C，无 IDF 依赖，可在宿主机单测）。
 *
 * F01 五状态可视化在 SSD1306 128×64 上的"变式"实现：
 * 状态词汇与 petlink 五态一一对应（枚举顺序刻意与 pl_status_t 相同，可直接强转）。
 *
 * 设计原则：
 *  - 只画内存帧缓冲 oui_fb，不碰硬件——怎么刷屏是 ssd1306.c 的事；
 *  - 屏幕即文档：每个测试界面（自检/扫描/状态）都是纯函数，喂 oui_disp_t 出整帧。
 */
#ifndef OLED_UI_H
#define OLED_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OUI_W        128
#define OUI_H        64
#define OUI_FB_SIZE  (OUI_W * OUI_H / 8)

/** 帧缓冲（SSD1306 原生页式：fb[page*128+x]，位 y%8，位0=该页最上行） */
extern uint8_t oui_fb[OUI_FB_SIZE];

/* 与 pl_status_t 同序（petlink.h）；_Static_assert 在 .c 里锚定 */
typedef enum {
  OUI_ST_CONNECTING = 0, /* 未握手 / 链路断：LINK.. */
  OUI_ST_THINKING,       /* 思考中：旋转指示 */
  OUI_ST_WORKING,        /* 执行工具中：方块翻转 + 进度条 */
  OUI_ST_WAIT_CONFIRM,   /* 等待授权：警告三角 + 闪烁 */
  OUI_ST_DONE,           /* 回合完成：大对勾 */
  OUI_ST_ERROR,          /* 出错：大叉 */
  OUI_ST_COUNT
} oui_status_t;

/** 状态主屏的一次性输入快照（所有指针字段由调用方保证本帧有效） */
typedef struct {
  oui_status_t state;
  uint32_t phase;      /* 动画相位，10Hz 递增（调用方维护） */
  uint32_t uptime_s;   /* 运行秒数（信息行显示） */
  int link_live;       /* 1=daemon 已握手，0=DEMO/断链 */
  int demo_run;        /* !link_live 时 demo 轮播是否在跑（右上角 LIVE/DEMO/IDLE） */
  const char *session; /* 可 NULL，截断 4 字符 */
  const char *label;   /* 协议 text 字段，可 NULL，超宽滚动 */
} oui_disp_t;

/* ---- 像素/文本原语（坐标越界自动裁剪） ---- */
void oui_clear(void);
void oui_px(int x, int y, int on);
int  oui_get(int x, int y);                          /* 供宿主机测试取像素 */
void oui_rect(int x0, int y0, int x1, int y1, int on); /* 实心矩形 */
void oui_line(int x0, int y0, int x1, int y1, int on, int thick);
void oui_text(int x, int y, const char *s, int scale, int on); /* scale 1/2 */
int  oui_text_w(const char *s, int scale);                     /* 居中排版用 */

/* ---- 整屏绘制（每函数产出一帧） ---- */
void oui_splash(const char *fw, const char *devid);
void oui_pixel_test(int phase);   /* 0=全亮 1=棋盘 2=全灭 —— 面板坏点/断码体检 */
void oui_i2c_scan(const uint8_t *addrs, int n); /* n≤12，两列小字 */
void oui_hw_info(uint32_t flash_mb, uint32_t psram_mb, uint32_t heap_kb, const char *idf_ver);
void oui_status(const oui_disp_t *d);

#ifdef __cplusplus
}
#endif
#endif /* OLED_UI_H */
