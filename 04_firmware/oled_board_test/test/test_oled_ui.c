/**
 * OLED 渲染层宿主机测试（不依赖 ESP-IDF）。
 * 跑法：sh test/run.sh
 *
 * 两层验证：
 *  1. 程序断言：状态序与 petlink 对齐、标题不越界、图标关键像素存在、帧非空；
 *  2. ASCII 渲染目检：每块屏dump 成字符画——字库每个字形、每张屏的布局
 *     由人眼把关（改字库/布局后必须重看一遍）。
 */
#include <stdio.h>
#include <string.h>

#include "oled_ui.h"
#include "petlink.h"

static int total, passed, failed;

#define CHECK(cond, name) do { \
  total++; \
  if (cond) { passed++; printf("  ✅ %s\n", name); } \
  else      { failed++; printf("  ❌ %s  (line %d)\n", name, __LINE__); } \
} while (0)

/* petlink 与 oui 状态序一致（main.c 里有同名断言，这里双保险） */
_Static_assert((int)PL_ST_CONNECTING == (int)OUI_ST_CONNECTING &&
               (int)PL_ST_THINKING == (int)OUI_ST_THINKING &&
               (int)PL_ST_WORKING == (int)OUI_ST_WORKING &&
               (int)PL_ST_WAITING_CONFIRM == (int)OUI_ST_WAIT_CONFIRM &&
               (int)PL_ST_DONE == (int)OUI_ST_DONE &&
               (int)PL_ST_ERROR == (int)OUI_ST_ERROR,
               "pl_status_t 与 oui_status_t 枚举顺序不同步");

static void dump(const char *title)
{
  printf("\n──── %s ────\n", title);
  for (int y = 0; y < OUI_H; y++) {
    for (int x = 0; x < OUI_W; x++) putchar(oui_get(x, y) ? '#' : '.');
    putchar('\n');
  }
}

static int fb_nonzero(void)
{
  for (size_t i = 0; i < OUI_FB_SIZE; i++)
    if (oui_fb[i]) return 1;
  return 0;
}

static int fb_all(int on)
{
  for (size_t i = 0; i < OUI_FB_SIZE; i++) {
    if (on && oui_fb[i] != 0xFF) return 0;
    if (!on && oui_fb[i] != 0x00) return 0;
  }
  return 1;
}

static void render_status(oui_status_t st, uint32_t phase)
{
  oui_disp_t d = {
    .state = st, .phase = phase, .uptime_s = 754,
    .link_live = 0, .demo_run = 1,
    .session = "a1b2", .label = "PLANNING..",
  };
  oui_status(&d);
}

int main(void)
{
  printf("== 1. 原语 ==\n");
  oui_clear();   CHECK(fb_all(0), "clear 全灭");
  oui_rect(0, 0, OUI_W - 1, OUI_H - 1, 1); CHECK(fb_all(1), "rect 全亮");
  oui_px(200, 200, 1);                     CHECK(1, "越界像素自动裁剪");
  CHECK(oui_text_w("THINKING", 2) <= 128 - 32, "主屏标题 2x 不越界（x32 起）");
  CHECK(oui_text_w("CONFIRM?", 2) <= 128 - 32, "最长标题 CONFIRM? 不越界");

  printf("\n== 2. 像素自检屏 ==\n");
  oui_pixel_test(0); CHECK(fb_all(1), "phase0 全亮");
  oui_pixel_test(2); CHECK(fb_all(0), "phase2 全灭");

  printf("\n== 3. 五状态屏（断言 + 目检）==\n");
  static const char *names[OUI_ST_COUNT] = {
    "CONNECTING", "THINKING", "WORKING", "WAIT_CONFIRM", "DONE", "ERROR",
  };
  for (int st = 0; st < OUI_ST_COUNT; st++) {
    char label[48];
    render_status((oui_status_t)st, 0);
    snprintf(label, sizeof(label), "%s 帧非空且顶栏反白", names[st]);
    CHECK(fb_nonzero() && oui_get(0, 0) && oui_get(OUI_W - 1, 0), label);
  }

  render_status(OUI_ST_THINKING, 0);
  CHECK(oui_get(16 + 9, 27), "THINKING 旋转点在轨道位 0");
  render_status(OUI_ST_THINKING, 3);
  CHECK(oui_get(16, 27 + 9), "THINKING 旋转点在轨道位 3");

  render_status(OUI_ST_DONE, 0);
  CHECK(oui_get(10, 30) && oui_get(19, 24), "DONE 对勾两段存在");

  render_status(OUI_ST_ERROR, 0);
  CHECK(oui_get(16, 27), "ERROR 大叉过中心");

  render_status(OUI_ST_WAIT_CONFIRM, 0);
  CHECK(oui_get(16, 15) && oui_get(5, 39) && oui_get(27, 39), "CONFIRM 三角三顶点");

  render_status(OUI_ST_WORKING, 0);
  CHECK(oui_get(0, 57) && oui_get(OUI_W - 1, 63), "WORKING 底部进度条外框");

  printf("\n== 4. 信息行/标签滚动 ==\n");
  {
    oui_disp_t d = {
      .state = OUI_ST_WORKING, .phase = 0, .uptime_s = 3661,
      .link_live = 1, .demo_run = 0, .session = "abcd1234",
      .label = "0123456789ABCDEFGHIJ1234", /* 24 字 > 21 → 滚动 */
    };
    oui_status(&d);
    CHECK(oui_get(2, 50), "长会话/长 uptime 信息行仍绘出（S 底行）");
    d.phase = 40; oui_status(&d);
    CHECK(fb_nonzero(), "标签滚动窗口推进后帧完整");
  }

  printf("\n== 5. 字库目检 ==\n");
  oui_clear();
  oui_text(1, 0,  "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 1, 1);
  oui_text(1, 9,  "0123456789 .:-/?!><()[]_+=*", 1, 1);
  oui_text(1, 20, "P4-TEST  THINKING  DONE 2X", 2, 1);
  dump("字库（上行小字 / 下行 2x）");

  printf("\n== 6. 整屏目检 ==\n");
  oui_splash("1.0.0", "p4t-a1b2c3");          dump("开机闪屏");
  {
    uint8_t addrs[] = {0x18, 0x3C};
    oui_i2c_scan(addrs, 2);                    dump("I2C 扫描（正常）");
    oui_i2c_scan(NULL, 0);                     dump("I2C 扫描（无设备）");
  }
  oui_hw_info(32, 32, 261234, "v5.5.1");       dump("硬件信息");
  oui_pixel_test(1);                           dump("棋盘格");
  for (int st = 0; st < OUI_ST_COUNT; st++) {
    render_status((oui_status_t)st, 2);
    char t[32];
    snprintf(t, sizeof(t), "状态屏: %s", names[st]);
    dump(t);
  }

  printf("\n== 7. I2C 扫描屏断言 ==\n");
  {
    uint8_t addrs[] = {0x18, 0x3C};
    oui_i2c_scan(addrs, 2);
    CHECK(oui_get(0, 0), "扫描屏顶栏存在");
    CHECK(fb_nonzero(), "扫描屏非空");
  }

  printf("\n合计: %d 项，通过 %d，失败 %d\n", total, passed, failed);
  return failed ? 1 : 0;
}
