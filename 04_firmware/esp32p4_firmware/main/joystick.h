/**
 * 摇杆导航（F17，v3.5 宪法 §4.8）：模拟摇杆 X 轴 → prev/next 方向事件。
 *
 * 硬件：复刻底板 5kΩ 自回中电位器摇杆，X 轴接 ADC 脚（Kconfig
 * PETLINK_GPIO_JOY_X，默认 -1 = 禁用）。Y 轴本期不接、SW 按下轴不接
 * （宪法 §4.8 明确边界）。
 *
 * 信号链：ADC oneshot 12bit → curve fitting 校准到毫伏 → 中心校准
 * （上电采 32 次均值，自回中有漂移，不能信理论中点）→ 归一化偏移
 * （-1000~+1000，左右对称）→ 死区 + 迟滞（进入 30% 触发沿、退回 18%
 * 复位，防临界抖动）→ 沿触发一次 + 按住每 400ms 自动重复。
 *
 * 纯逻辑（joy_axis_t 状态机）与硬件读数分离——状态机可在宿主机单测
 * （test/test_joystick.c），ADC 采样子程序注入。
 *
 * 失败兜底：ADC 初始化失败只记日志返回 false，摇杆不可用绝不拖垮
 * 按键授权主链路（与 F14/F16 同哲学）。
 */
#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdbool.h>
#include <stdint.h>

#include "petlink.h"

/** 一个轴的导航状态机（纯逻辑，可宿主机单测） */
typedef struct {
  /* 配置（init 时定死） */
  int32_t enter;  /* 进入阈值（千分比 ×10，见下） */
  int32_t exit;   /* 退出阈值 */
  /* 状态 */
  int32_t center; /* 中心校准值（毫伏） */
  int32_t half;   /* 半量程（毫伏，中心到两端的平均距离） */
  int armed;      /* 迟滞状态：0=中位待命，-1=左已触发，+1=右已触发 */
  uint32_t t_last; /* 最近一次触发/重复时刻（ms，外部喂） */
} joy_axis_t;

/**
 * 轴状态机初始化。
 * enter/exit 用满量程千分比（如 300/180 = 进入 30%、退出 18%）。
 * center/half 由校准过程填（joy_calibrate），此处只清状态。
 */
void joy_axis_init(joy_axis_t *a, int enter_pm, int exit_pm);

/**
 * 喂一个校准后的毫伏读数，返回导航事件。
 * @return 0=无事件，-1=prev（左），+1=next（右）
 * now_ms 由外部喂（协议任务的 io_uptime），保证可测。
 */
int joy_axis_feed(joy_axis_t *a, int32_t mv, uint32_t now_ms);

/* ---------------- 以下为固件侧硬件装配（宿主机测试/未配脚时不编译） ---------------- */

/* 宿主机测试没有 sdkconfig.h：CONFIG 宏未定义 → 走 #if 的未定义=0 规则跳过；
 * 固件配 -1（禁用）同样不编译——joystick_init 只在配了引脚时存在，
 * 调用方（main.c）用同一条件门控 */
#if defined(CONFIG_PETLINK_GPIO_JOY_X) && CONFIG_PETLINK_GPIO_JOY_X >= 0

#include "driver/gpio.h"

/**
 * 初始化摇杆 ADC（X 轴）。gpio<0 或 ADC 失败返回 false（摇杆禁用，
 * 主链路不受影响）。center/half 在返回 true 前完成中心校准。
 */
bool joystick_init(joy_axis_t *x_axis);

/** 采一次 X 轴（4 次均值），返回校准毫伏值；失败返回 INT32_MIN */
int32_t joystick_read_x(void);

#endif /* 硬件装配段 */

#endif /* JOYSTICK_H */
