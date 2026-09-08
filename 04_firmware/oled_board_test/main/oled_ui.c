/**
 * OLED 渲染层实现。接口语义见 oled_ui.h。
 *
 * 字库为本项目自绘 5×7 点阵（列字节、位0=最上行），字形以宿主机测试
 * （test/test_oled_ui.c 的 ASCII 渲染输出）逐字目检过——不依赖任何第三方字库。
 */
#include "oled_ui.h"

#include <stdio.h>
#include <string.h>

uint8_t oui_fb[OUI_FB_SIZE];

/* ---------------- 5×7 字库：0x20..0x5F，小写渲染为大写 ---------------- */
/* 列字节 × 5，位0=顶行；未定义到位的字形同样经宿主机目检 */
static const uint8_t K_FONT[64][5] = {
  {0x00,0x00,0x00,0x00,0x00}, /*   */
  {0x00,0x00,0x5D,0x00,0x00}, /* ! */
  {0x07,0x00,0x00,0x00,0x07}, /* " */
  {0x0A,0x1F,0x0A,0x1F,0x0A}, /* # */
  {0x24,0x2A,0x5B,0x32,0x20}, /* $ */
  {0x21,0x42,0x04,0x42,0x21}, /* % */
  {0x32,0x49,0x24,0x49,0x3E}, /* & */
  {0x05,0x03,0x00,0x00,0x00}, /* ' */
  {0x00,0x3E,0x41,0x41,0x00}, /* ( */
  {0x00,0x41,0x41,0x3E,0x00}, /* ) */
  {0x04,0x15,0x0E,0x15,0x04}, /* * */
  {0x08,0x08,0x3E,0x08,0x08}, /* + */
  {0x00,0x00,0x00,0x60,0x40}, /* , */
  {0x08,0x08,0x08,0x08,0x08}, /* - */
  {0x00,0x00,0x00,0x60,0x60}, /* . */
  {0x20,0x10,0x08,0x04,0x02}, /* / */
  {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
  {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
  {0x42,0x61,0x51,0x49,0x46}, /* 2 */
  {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
  {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
  {0x27,0x45,0x45,0x45,0x39}, /* 5 */
  {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
  {0x01,0x71,0x09,0x05,0x03}, /* 7 */
  {0x36,0x49,0x49,0x49,0x36}, /* 8 */
  {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
  {0x00,0x36,0x36,0x00,0x00}, /* : */
  {0x00,0x36,0x36,0x00,0x20}, /* ; */
  {0x08,0x14,0x22,0x41,0x00}, /* < */
  {0x14,0x14,0x14,0x14,0x14}, /* = */
  {0x41,0x22,0x14,0x08,0x00}, /* > */
  {0x02,0x01,0x51,0x09,0x06}, /* ? */
  {0x3E,0x41,0x5D,0x55,0x1E}, /* @ */
  {0x7E,0x11,0x11,0x11,0x7E}, /* A */
  {0x7F,0x49,0x49,0x49,0x36}, /* B */
  {0x3E,0x41,0x41,0x41,0x22}, /* C */
  {0x7F,0x41,0x41,0x22,0x1C}, /* D */
  {0x7F,0x49,0x49,0x49,0x41}, /* E */
  {0x7F,0x09,0x09,0x09,0x01}, /* F */
  {0x3E,0x41,0x49,0x49,0x7A}, /* G */
  {0x7F,0x08,0x08,0x08,0x7F}, /* H */
  {0x00,0x41,0x7F,0x41,0x00}, /* I */
  {0x20,0x40,0x41,0x3F,0x01}, /* J */
  {0x7F,0x08,0x14,0x22,0x41}, /* K */
  {0x7F,0x40,0x40,0x40,0x40}, /* L */
  {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
  {0x7F,0x04,0x08,0x10,0x7F}, /* N */
  {0x3E,0x41,0x41,0x41,0x3E}, /* O */
  {0x7F,0x09,0x09,0x09,0x06}, /* P */
  {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
  {0x7F,0x09,0x19,0x29,0x46}, /* R */
  {0x46,0x49,0x49,0x49,0x31}, /* S */
  {0x01,0x01,0x7F,0x01,0x01}, /* T */
  {0x3F,0x40,0x40,0x40,0x3F}, /* U */
  {0x1F,0x20,0x40,0x20,0x1F}, /* V */
  {0x3F,0x40,0x38,0x40,0x3F}, /* W */
  {0x63,0x14,0x08,0x14,0x63}, /* X */
  {0x07,0x08,0x70,0x08,0x07}, /* Y */
  {0x61,0x51,0x49,0x45,0x43}, /* Z */
  {0x7F,0x41,0x41,0x41,0x00}, /* [ */
  {0x02,0x04,0x08,0x10,0x20}, /* \ */
  {0x41,0x41,0x41,0x7F,0x00}, /* ] */
  {0x04,0x02,0x03,0x02,0x04}, /* ^ */
  {0x40,0x40,0x40,0x40,0x40}, /* _ */
};

/* ---------------- 像素原语 ---------------- */

void oui_clear(void) { memset(oui_fb, 0, sizeof(oui_fb)); }

void oui_px(int x, int y, int on)
{
  if ((unsigned)x >= OUI_W || (unsigned)y >= OUI_H) return;
  uint8_t *b = &oui_fb[(y >> 3) * OUI_W + x];
  uint8_t m = (uint8_t)(1u << (y & 7));
  if (on) *b |= m; else *b &= (uint8_t)~m;
}

int oui_get(int x, int y)
{
  if ((unsigned)x >= OUI_W || (unsigned)y >= OUI_H) return 0;
  return (oui_fb[(y >> 3) * OUI_W + x] >> (y & 7)) & 1;
}

void oui_rect(int x0, int y0, int x1, int y1, int on)
{
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      oui_px(x, y, on);
}

void oui_line(int x0, int y0, int x1, int y1, int on, int thick)
{
  for (int i = 0; i < thick; i++)
    for (int j = 0; j < thick; j++) {
      int sx = x0 + i, sy = y0 + j, ex = x1 + i, ey = y1 + j;
      int dx = ex > sx ? ex - sx : sx - ex;
      int dy = ey > sy ? ey - sy : sy - ey;
      int sxm = sx < ex ? 1 : -1, sym = sy < ey ? 1 : -1;
      int err = dx - dy;
      for (;;) {
        oui_px(sx, sy, on);
        if (sx == ex && sy == ey) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; sx += sxm; }
        if (e2 <  dx) { err += dx; sy += sym; }
      }
    }
}

/* ---------------- 文本 ---------------- */

static void oui_char(int x, int y, char c, int sc, int on)
{
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c < ' ' || c > '_') c = '?';
  const uint8_t *g = K_FONT[c - ' '];
  for (int col = 0; col < 5; col++)
    for (int r = 0; r < 7; r++)
      if ((g[col] >> r) & 1)
        oui_rect(x + col * sc, y + r * sc, x + col * sc + sc - 1,
                 y + r * sc + sc - 1, on);
}

void oui_text(int x, int y, const char *s, int sc, int on)
{
  if (!s || sc < 1) return;
  for (; *s; s++, x += 6 * sc) oui_char(x, y, *s, sc, on);
}

int oui_text_w(const char *s, int sc)
{
  if (!s || !*s) return 0;
  return (int)strlen(s) * 6 * sc - sc; /* 末字符不带间隔 */
}

static void oui_text_center(int y, const char *s, int sc, int on)
{
  oui_text((OUI_W - oui_text_w(s, sc)) / 2, y, s, sc, on);
}

/* ---------------- 屏幕通用件 ---------------- */

/** 顶部反白条：标题居左、状态角标居右 */
static void header(const char *title, const char *right)
{
  oui_rect(0, 0, OUI_W - 1, 7, 1);
  oui_text(2, 1, title, 1, 0);
  if (right)
    oui_text(OUI_W - oui_text_w(right, 1) - 2, 1, right, 1, 0);
}

/* ---------------- 整屏 ---------------- */

void oui_splash(const char *fw, const char *devid)
{
  oui_clear();
  oui_rect(0, 0, OUI_W - 1, OUI_H - 1, 1);
  oui_rect(2, 2, OUI_W - 3, OUI_H - 3, 0);
  oui_text_center(6,  "ESP32-P4 BOARD TEST", 1, 1);
  oui_text_center(16, "OLED", 2, 1);
  oui_text_center(34, "SSD1306 128X64", 1, 1);
  char row[24];
  snprintf(row, sizeof(row), "FW %s", fw ? fw : "?");
  oui_text_center(46, row, 1, 1);
  snprintf(row, sizeof(row), "ID %s", devid ? devid : "?");
  oui_text_center(56, row, 1, 1);
}

void oui_pixel_test(int phase)
{
  oui_clear();
  if (phase == 0) {
    oui_rect(0, 0, OUI_W - 1, OUI_H - 1, 1);           /* 全亮 */
  } else if (phase == 1) {
    for (int py = 0; py < OUI_H; py += 4)              /* 4px 棋盘 */
      for (int px = 0; px < OUI_W; px += 4)
        if (((px >> 2) + (py >> 2)) & 1)
          oui_rect(px, py, px + 3, py + 3, 1);
  }
}

void oui_i2c_scan(const uint8_t *addrs, int n)
{
  oui_clear();
  header("I2C SCAN", NULL);
  char cell[16];
  for (int i = 0; i < n && i < 10; i++) {
    const char *name = "DEV";
    if (addrs[i] == 0x3C) name = "OLED";
    else if (addrs[i] == 0x18) name = "ES8311";
    snprintf(cell, sizeof(cell), "%02X %s", addrs[i], name);
    oui_text(2 + (i & 1) * 64, 12 + (i >> 1) * 10, cell, 1, 1);
  }
  if (n > 10) {
    int extra = n - 10; /* 钳到两位数：屏幕一格也只放得下这么多 */
    if (extra > 99) extra = 99;
    snprintf(cell, sizeof(cell), "+%d MORE", extra);
    oui_text(2 + ((n - 1) & 1) * 64, 12 + ((n - 1) >> 1) * 10, cell, 1, 1);
  }
  if (n == 0)
    oui_text_center(40, "NO DEVICE !!", 1, 1);
  else {
    char foot[16];
    snprintf(foot, sizeof(foot), "%d DEV FOUND", n);
    oui_text_center(57, foot, 1, 1);
  }
}

void oui_hw_info(uint32_t flash_mb, uint32_t psram_mb, uint32_t heap_kb,
                 const char *idf_ver)
{
  oui_clear();
  header("HW INFO", NULL);
  char row[40]; /* "FLASH %luM  PSRAM %luM" 最大 37 字节，26 会被判截断 */
  if (psram_mb)
    snprintf(row, sizeof(row), "FLASH %luM  PSRAM %luM",
             (unsigned long)flash_mb, (unsigned long)psram_mb);
  else
    snprintf(row, sizeof(row), "FLASH %luM  PSRAM --",
             (unsigned long)flash_mb);
  oui_text_center(16, row, 1, 1);
  snprintf(row, sizeof(row), "HEAP %luK", (unsigned long)heap_kb);
  oui_text_center(30, row, 1, 1);
  snprintf(row, sizeof(row), "IDF %s", idf_ver ? idf_ver : "?");
  oui_text_center(44, row, 1, 1);
}

/* ---------------- 状态主屏 ---------------- */

static const char *K_TITLES[OUI_ST_COUNT] = {
  "LINK..", "THINKING", "WORKING", "CONFIRM?", "DONE", "ERROR",
};

/* 图标区：x 4..29，y 15..40（26×26） */

static const int8_t K_ORBIT[8][2] = { /* 自绘 8 相位圆轨道（r=9） */
  {9, 0}, {6, 6}, {0, 9}, {-6, 6}, {-9, 0}, {-6, -6}, {0, -9}, {6, -6},
};

static void icon_connecting(uint32_t phase)
{
  int off = (int)((phase >> 1) & 3) * 2;      /* 双箭头相向滑动 */
  oui_text(4 + off, 21, ">", 2, 1);
  oui_text(19 - off, 21, "<", 2, 1);
}

static void icon_thinking(uint32_t phase)
{
  int i = (int)(phase & 7);
  for (int k = 0; k < 3; k++) {               /* 当前点 + 两级拖尾 */
    const int8_t *p = K_ORBIT[(i - k) & 7];
    int sz = 3 - k, half = sz / 2;
    oui_rect(16 + p[0] - half, 27 + p[1] - half,
             16 + p[0] - half + sz - 1, 27 + p[1] - half + sz - 1, 1);
  }
}

static void icon_working(uint32_t phase)
{
  if (phase & 2) {                            /* 方块 ⇄ 菱形交替 = 翻转感 */
    oui_rect(7, 18, 25, 36, 0);
    oui_rect(7, 18, 25, 18, 1); oui_rect(7, 36, 25, 36, 1);
    oui_rect(7, 18, 7, 36, 1);  oui_rect(25, 18, 25, 36, 1);
  } else {
    oui_line(16, 17, 26, 27, 1, 1); oui_line(26, 27, 16, 37, 1, 1);
    oui_line(16, 37, 6, 27, 1, 1);  oui_line(6, 27, 16, 17, 1, 1);
  }
}

static void icon_confirm(uint32_t phase)
{
  oui_line(16, 15, 5, 39, 1, 2);              /* 警告三角 */
  oui_line(5, 39, 27, 39, 1, 2);
  oui_line(27, 39, 16, 15, 1, 2);
  if (((phase >> 1) & 1) == 0) {              /* 感叹号闪烁 */
    oui_rect(15, 23, 16, 30, 1);
    oui_rect(15, 33, 16, 34, 1);
  }
}

static void icon_done(void)
{
  oui_line(6, 26, 13, 33, 1, 2);              /* 大对勾 */
  oui_line(13, 33, 24, 16, 1, 2);
}

static void icon_error(uint32_t phase)
{
  if (((phase >> 1) & 1) == 0) {              /* 大叉闪烁 */
    oui_line(7, 16, 26, 39, 1, 2);
    oui_line(26, 16, 7, 39, 1, 2);
  }
}

/** 底部动效：working 扫描条；thinking/connecting 波浪点 */
static void footer_anim(oui_status_t st, uint32_t phase)
{
  if (st == OUI_ST_WORKING) {
    oui_rect(0, 57, OUI_W - 1, 63, 0);
    oui_rect(0, 57, OUI_W - 1, 57, 1); oui_rect(0, 63, OUI_W - 1, 63, 1);
    oui_rect(0, 58, 0, 62, 1);        oui_rect(OUI_W - 1, 58, OUI_W - 1, 62, 1);
    int p = (int)(phase % 20);
    int frac = p < 10 ? p : 19 - p;   /* 0..9 往返 */
    int fill = 12 * frac;             /* 0..108 */
    if (fill > 0) oui_rect(2, 59, 2 + fill - 1, 61, 1);
  } else if (st == OUI_ST_THINKING || st == OUI_ST_CONNECTING) {
    for (int i = 0; i < 3; i++)
      if ((int)(((phase + 8 - i * 2) & 7)) < 3)
        oui_rect(58 + i * 5, 59, 60 + i * 5, 61, 1);
  }
}

void oui_status(const oui_disp_t *d)
{
  oui_clear();
  header("P4-TEST", d->link_live ? "LIVE" : (d->demo_run ? "DEMO" : "IDLE"));

  oui_status_t st = d->state;
  if (st < 0 || st >= OUI_ST_COUNT) st = OUI_ST_ERROR;

  switch (st) {
    case OUI_ST_CONNECTING:    icon_connecting(d->phase); break;
    case OUI_ST_THINKING:      icon_thinking(d->phase); break;
    case OUI_ST_WORKING:       icon_working(d->phase); break;
    case OUI_ST_WAIT_CONFIRM:  icon_confirm(d->phase); break;
    case OUI_ST_DONE:          icon_done(); break;
    case OUI_ST_ERROR:         icon_error(d->phase); break;
    default: break;
  }

  oui_text(32, 21, K_TITLES[st], 2, 1);

  char row[26];
  char sess[8] = "----";
  if (d->session && d->session[0]) {
    size_t i;
    for (i = 0; i < 4 && d->session[i]; i++) sess[i] = d->session[i];
    sess[i] = '\0';
  }
  snprintf(row, sizeof(row), "S:%s U:%lu:%02lu", sess,
           (unsigned long)(d->uptime_s / 60), (unsigned long)(d->uptime_s % 60));
  oui_text(2, 44, row, 1, 1);

  if (d->label && d->label[0]) {              /* 长标签滚动显示 */
    char buf[24];
    size_t len = 0;
    for (; d->label[len] && len < sizeof(buf) - 1; len++)
      buf[len] = d->label[len];
    buf[len] = '\0';
    int maxch = 21;
    if ((int)len <= maxch) {
      oui_text(2, 52, buf, 1, 1);
    } else {
      int win = (int)len - maxch;
      int start = (int)((d->phase >> 2) % (uint32_t)(win + 1));
      oui_text(2, 52, buf + start, 1, 1);
    }
  }

  footer_anim(st, d->phase);
}
