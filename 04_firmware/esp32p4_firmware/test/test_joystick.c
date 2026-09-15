/**
 * 摇杆轴状态机宿主机单测（F17，v3.5）：死区/迟滞/自动重复/中心校准。
 * 跑法：sh test/run.sh（无 IDF 依赖）
 * 覆盖：中位静默、越阈触发、迟滞防抖（临界徘徊不连发）、
 *       按住自动重复节奏、左右对称、漂移中心下仍正常。
 */
#include <stdio.h>

#include "joystick.h"

static int total, passed, failed;

#define CHECK(cond, name) do { \
  total++; \
  if (cond) { passed++; printf("  ✅ %s\n", name); } \
  else      { failed++; printf("  ❌ %s  (line %d)\n", name, __LINE__); } \
} while (0)

int main(void) {
  joy_axis_t ax;

  printf("== 1. 中位静默 ==\n");
  {
    joy_axis_init(&ax, 300, 180);
    ax.center = 1650; ax.half = 1500; /* 典型校准值 */
    CHECK(joy_axis_feed(&ax, 1650, 0) == 0, "正中无事件");
    CHECK(joy_axis_feed(&ax, 1700, 10) == 0, "死区内小幅漂移无事件");
    CHECK(joy_axis_feed(&ax, 2050, 20) == 0, "29%% 偏移仍在死区内（<30%%）");
  }

  printf("== 2. 越阈触发 ==\n");
  {
    joy_axis_init(&ax, 300, 180);
    ax.center = 1650; ax.half = 1500;
    CHECK(joy_axis_feed(&ax, 2100, 0) == 1, "右推 30%% → next（+1）");
    CHECK(joy_axis_feed(&ax, 2200, 50) == 0, "持续按住 50ms < 400ms 不重复");
    CHECK(joy_axis_feed(&ax, 2200, 450) == 1, "按住 450ms → 自动重复 next");
    CHECK(joy_axis_feed(&ax, 2200, 500) == 0, "重复后立即再喂不连发");
    CHECK(joy_axis_feed(&ax, 2200, 860) == 1, "再过 400ms → 又一次 next");
    CHECK(joy_axis_feed(&ax, 1200, 900) == -1, "从右直甩左端一拍：换向直接报 prev（不用先回中）");
  }

  printf("== 3. 迟滞防抖（临界徘徊不连发） ==\n");
  {
    joy_axis_init(&ax, 300, 180);
    ax.center = 1650; ax.half = 1500;
    CHECK(joy_axis_feed(&ax, 2200, 0) == 1, "右推触发 next");
    /* 在 25% 处徘徊：低于 enter(30%) 但高于 exit(18%)——armed 保持，不重触发 */
    CHECK(joy_axis_feed(&ax, 2025, 100) == 0, "回落到 25%%：迟滞区无事件");
    CHECK(joy_axis_feed(&ax, 2200, 100) == 0, "回推到 37%%：同 armed 且 100ms 内不重复");
    CHECK(joy_axis_feed(&ax, 1900, 600) == 0, "歇在 16.7%% < 18%%：armed 复位（无事件）");
    CHECK(joy_axis_feed(&ax, 1870, 700) == 0, "停在 14.7%%：仍复位态（无事件）");
    CHECK(joy_axis_feed(&ax, 2200, 750) == 1, "复位后再推右 → 重新触发 next");
  }

  printf("== 4. 左右对称 ==\n");
  {
    joy_axis_init(&ax, 300, 180);
    ax.center = 1650; ax.half = 1500;
    CHECK(joy_axis_feed(&ax, 1200, 0) == -1, "左推 30%% → prev（-1）");
    CHECK(joy_axis_feed(&ax, 1150, 410) == -1, "按住左 → 自动重复 prev");
    CHECK(joy_axis_feed(&ax, 1700, 450) == 0, "回中（armed 复位，无事件）");
    CHECK(joy_axis_feed(&ax, 2200, 500) == 1, "复位后右推 → next（不是 prev）");
  }

  printf("== 5. 漂移中心（校准值非理论中点） ==\n");
  {
    joy_axis_init(&ax, 300, 180);
    ax.center = 1780; ax.half = 1500; /* 上电校准发现中心偏右 130mV */
    CHECK(joy_axis_feed(&ax, 1780, 0) == 0, "漂移中心处无事件");
    CHECK(joy_axis_feed(&ax, 2230, 0) == 1, "右推 30%%（按漂移中心算）→ next");
    CHECK(joy_axis_feed(&ax, 1330, 10) == -1, "左推 30%%（按漂移中心算）→ prev");
  }

  printf("== 6. 初始化安全性 ==\n");
  {
    joy_axis_init(&ax, 300, 180);
    /* 未校准（center=0 half=1）：读数饱和归一化，状态机不崩不除零 */
    CHECK(joy_axis_feed(&ax, -2000, 0) == -1, "未校准时极左读数 → prev（不除零不越界）");
    joy_axis_init(&ax, 300, 180);
    ax.center = 1650; ax.half = 1500;
    CHECK(joy_axis_feed(&ax, 1650, 0) == 0, "重新 init 后状态清零");
  }

  printf("\n摇杆状态机测试：%d/%d 通过\n", passed, total);
  return failed ? 1 : 0;
}
