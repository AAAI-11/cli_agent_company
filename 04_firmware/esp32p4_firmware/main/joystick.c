/**
 * 摇杆导航实现（F17，v3.5 宪法 §4.8）。
 *
 * 文件分两段：
 *  - 纯逻辑状态机（joy_axis_*）：死区/迟滞/自动重复，无任何硬件依赖，
 *    宿主机单测覆盖（test/test_joystick.c）
 *  - 硬件装配（Kconfig 配脚才编译，门控同 mic.c）：ADC oneshot + curve
 *    fitting 校准 + 中心校准 + 4 次均值采样
 *
 * 自动重复节奏：越死区沿立刻触发一次；持续按住每 JOY_REPEAT_MS 再触发
 * （不渐进加速——列表最多 6 个气泡，匀速翻足够）。
 */
#include "joystick.h"

#include <stddef.h>

/* ---------------- 纯逻辑：轴导航状态机 ---------------- */

/* 满量程千分比 → 内部整数比较。偏移归一化为 -1000~+1000。 */
#define JOY_REPEAT_MS 400 /* 按住自动重复周期 */

void joy_axis_init(joy_axis_t *a, int enter_pm, int exit_pm)
{
  a->enter = enter_pm;
  a->exit = exit_pm;
  a->center = 0;
  a->half = 1; /* 防 0 除：校准前 feed 也安全 */
  a->armed = 0;
  a->t_last = 0;
}

int joy_axis_feed(joy_axis_t *a, int32_t mv, uint32_t now_ms)
{
  /* 归一化偏移：中心为 0，两端 ±1000（half 为中心到端点的量程） */
  int32_t off = (mv - a->center) * 1000 / a->half;

  /* 回中复位：|偏移| 低于退出阈值（迟滞下沿）——最先判，任何 armed 都清 */
  if (off > -a->exit && off < a->exit) {
    a->armed = 0;
    return 0;
  }

  /* 右触发区：越进入阈值 → 触发沿（从中位来，或从左区直甩换向） */
  if (off >= a->enter) {
    if (a->armed <= 0) {
      a->armed = 1;
      a->t_last = now_ms;
      return 1; /* next（右推） */
    }
    if (now_ms - a->t_last >= JOY_REPEAT_MS) { /* 已在右区：按住自动重复 */
      a->t_last = now_ms;
      return 1;
    }
    return 0;
  }
  /* 左触发区（对称） */
  if (off <= -a->enter) {
    if (a->armed >= 0) {
      a->armed = -1;
      a->t_last = now_ms;
      return -1; /* prev（左推） */
    }
    if (now_ms - a->t_last >= JOY_REPEAT_MS) {
      a->t_last = now_ms;
      return -1;
    }
    return 0;
  }
  /* 迟滞区（exit ≤ |off| < enter）：保持 armed、不触发不重复——
   * 停在这里（死区边缘歇手）绝不能继续自动重复 */
  return 0;
}

/* ---------------- 硬件装配（仅固件构建） ---------------- */

#if defined(CONFIG_PETLINK_GPIO_JOY_X) && CONFIG_PETLINK_GPIO_JOY_X >= 0

#include <stdio.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "joystick";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

/* 上电中心校准：N 次采样取均值。摇杆须处于自然中位（上电即是）。
 * 同时探测量程：中心 ± 少量采样找两端趋势不可靠，改为固定满量程
 * 3300mV 的一半按衰减系数折算——ADC_ATTEN_DB_12 实测线性区约
 * 100~3100mV，half 取 1500mV（中心 1650 附近，左右各到 ~150）。
 * 宁可阈值难触发一点，不可轻推即触发。 */
#define JOY_CAL_N     32
#define JOY_HALF_MV   1500

int32_t joystick_read_x(void)
{
  int sum = 0;
  for (int i = 0; i < 4; i++) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
      return INT32_MIN;
    }
    sum += mv;
  }
  return sum / 4;
}

bool joystick_init(joy_axis_t *x_axis)
{
  int gpio = CONFIG_PETLINK_GPIO_JOY_X; /* 编译期门控保证 >= 0 */

  adc_unit_t unit;
  if (adc_oneshot_io_to_channel(gpio, &unit, &s_chan) != ESP_OK) {
    ESP_LOGW(TAG, "GPIO%d 不是 ADC 脚——摇杆禁用", gpio);
    return false;
  }

  adc_oneshot_unit_init_cfg_t init = { .unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE };
  if (adc_oneshot_new_unit(&init, &s_adc) != ESP_OK) {
    ESP_LOGW(TAG, "ADC 单元初始化失败——摇杆禁用");
    return false;
  }
  adc_oneshot_chan_cfg_t cfg = {
    .atten = ADC_ATTEN_DB_12,   /* 满量程覆盖摇杆 0~3.3V 电位器 */
    .bitwidth = ADC_BITWIDTH_12,
  };
  if (adc_oneshot_config_channel(s_adc, s_chan, &cfg) != ESP_OK) {
    ESP_LOGW(TAG, "ADC 通道配置失败——摇杆禁用");
    return false;
  }

  /* 校准句柄：P4 支持 curve fitting；eFuse 未烧则降级跳过（raw≈mv 也能用） */
  adc_cali_curve_fitting_config_t cali_cfg = {
    .unit_id = unit,
    .chan = s_chan,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  s_cali = NULL;
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
    ESP_LOGW(TAG, "ADC 校准不可用（eFuse 未烧）——用原始读数近似");
  }

  /* 中心校准：上电摇杆自然回中，32 次均值 */
  int64_t acc = 0;
  int ok_n = 0;
  for (int i = 0; i < JOY_CAL_N; i++) {
    int32_t mv = joystick_read_x();
    if (mv == INT32_MIN) return false;
    acc += mv;
    ok_n++;
  }
  x_axis->center = (int32_t)(acc / ok_n);
  x_axis->half = JOY_HALF_MV;
  ESP_LOGI(TAG, "摇杆就绪：X=GPIO%d 中心 %dmV 半量程 %dmV（ADC%d ch%d%s）",
           gpio, (int)x_axis->center, (int)x_axis->half, (int)unit + 1, (int)s_chan,
           s_cali ? " curve-fitting 校准" : "");
  return true;
}

#endif /* 硬件装配段 */
