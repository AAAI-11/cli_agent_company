/**
 * petlink-fw v0.4 —— ESP32-P4 固件入口（屏幕 + 语音输入版）。
 *
 * 链路：UART0 @921600（板上 USB-C 经 CH343 桥接到 UART0——微雪 P4-WIFI6 的
 *       实际布线，P4 原生 USJ 未引出；daemon 的 UsbCdcLink 按 WCH VID 1a86 识别。
 *       921600 = 语音上行带宽：16kHz×16bit = 32KB/s，115200 只有 ~11.5KB/s）
 * 按键：YES / NO 两键（GPIO menuconfig 可改），一脚接 GPIO 一脚接 GND，内部上拉；
 *       YES 短按 <600ms = 放行、长按 ≥600ms = 拒绝；NO 键按下即拒（v0.1.1 增补，
 *       规避演示现场长按 600ms 的手感风险——2026-09-03 真机两次没按准的教训）
 * 语音：v0.4 空闲态长按 YES = 板载 PDM 麦录音 → audio 帧上行（mic.c，协议 §3.7）；
 *       授权等待（waiting_confirm）时长按仍是拒绝手势，不触发录音
 * UI  ：MIPI-DSI 480×640 + LVGL 六态宠物（v0.2 起接管 ui_on_state——快照推送，
 *       渲染全在 LVGL 任务，petlink 协议核心零改动）
 *
 * 任务模型：协议任务 20ms 一拍（读 CDC → 喂协议核心 → 消抖按键 → 心跳/判活）
 * + mic 录音任务（事件触发）。UART TX 互斥锁保证两任务并发发帧不劈行。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#include "petlink.h"
#include "disp_dsi.h"
#include "pet_ui.h"
#include "mic.h"

static const char *TAG = "petlink";

/* ---------------- 协议核心的 IO 绑定 ---------------- */

static pl_t g_pl;
static SemaphoreHandle_t s_tx_mu; /* UART TX 互斥：协议任务与 mic 任务并发出站 */

static void io_send(const char *line, void *ctx)
{
  (void)ctx;
  /* 整行原子写出：两个任务并发发帧时绝不互相劈行 */
  xSemaphoreTake(s_tx_mu, portMAX_DELAY);
  uart_write_bytes(UART_NUM_0, line, strlen(line));
  uart_write_bytes(UART_NUM_0, "\n", 1);
  xSemaphoreGive(s_tx_mu);
}

static uint32_t io_uptime(void *ctx)
{
  (void)ctx;
  return (uint32_t)(esp_timer_get_time() / 1000);
}

/* UI 钩子：v0.2 屏幕接管——快照推送给 LVGL 任务渲染（text 暂不上屏：
 * 字体是 21 符号子集，任意中文会显 '?'，v0.3 素材/文本槽再扩） */
static char g_last_sess[16] = "";
static pl_status_t g_last_status = PL_ST_CONNECTING; /* 语音输入的门控用（voice_armed） */

/* 截到容量上限且不劈开 UTF-8 多字节字符（截在续字节上就回退到字符起点） */
static void copy_utf8_bounded(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  memcpy(dst, src ? src : "", n);
  dst[n] = '\0';
}

static void ui_on_state(pl_status_t st, const char *session, const char *text, void *ctx)
{
  (void)ctx;
  g_last_status = st;
  ESP_LOGI(TAG, "[ui] %s  session=%s  text=%s",
           pl_status_name(st), session ? session : "-", text ? text : "-");
  pet_ui_snap_t snap = {
    .state = st,
    .link_live = g_pl.helloed,
  };
  snprintf(snap.session, sizeof(snap.session), "%s", session ? session : "");
  snprintf(g_last_sess, sizeof(g_last_sess), "%s", session ? session : "");
  copy_utf8_bounded(snap.text, sizeof(snap.text), text);
  pet_ui_push(&snap);
}

static void ui_on_hello_ok(const char *daemon_ver, void *ctx)
{
  (void)ctx;
  ESP_LOGI(TAG, "[ui] 握手完成 daemon=%s", daemon_ver);
  pet_ui_link(true);
  /* 掉线沿曾把 UI 推回 CONNECTING，pet_ui_link 只点亮 link_live 不改 state，
   * 不主动恢复就永远卡"正在连接"（2026-09-08 真机教训）。回空闲绿脸；
   * 若 daemon 侧会话仍在进行，由 daemon 握手后重推 state 覆盖 */
  pet_ui_snap_t snap = {
    .state = PL_ST_DONE,
    .link_live = true,
  };
  pet_ui_push(&snap);
}

/* ---------------- 实体按键：消抖 + 短/长按（YES/NO 共用一套判型） ---------------- */

#define DEBOUNCE_MS 20
#define LONGPRESS_MS 600

typedef struct {
  gpio_num_t gpio;
  const char *key;   /* PetLink button 帧的 key 名 */
  const char *label; /* 日志标签 */
  int stable;        /* 稳定电平：1=松开（上拉），0=按下 */
  int last_raw;      /* 最近原始电平 */
  uint32_t t_raw;    /* 原始电平跳变时刻（消抖观察窗起点） */
  uint32_t t_press;  /* 按下起点 */
} btn_t;

static bool g_mic_ok; /* 语音输入可用（编译开 + mic_init 成功） */

static btn_t g_yes = { .key = "yes", .label = "YES", .stable = 1, .last_raw = 1 };
static btn_t g_no  = { .key = "no",  .label = "NO",  .stable = 1, .last_raw = 1 };

static void btn_init(btn_t *b, gpio_num_t gpio)
{
  b->gpio = gpio;
  gpio_reset_pin(gpio);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
}

/* 语音输入武装条件：编译开 + 初始化过 + 已握手 + 不在授权等待 + 不忙 */
static bool voice_armed(void)
{
  return g_mic_ok && g_pl.helloed &&
         g_last_status != PL_ST_WAITING_CONFIRM && !mic_busy();
}

static void btn_poll(btn_t *b, uint32_t now)
{
  int raw = gpio_get_level(b->gpio);
  if (raw != b->last_raw) {          /* 电平跳了：重开观察窗 */
    b->last_raw = raw;
    b->t_raw = now;
    return;
  }
  if (now - b->t_raw < DEBOUNCE_MS) return; /* 窗内不出结论 */
  if (raw == b->stable) return;      /* 结论与现状一致，无事 */

  b->stable = raw;
  if (raw == 0) {
    b->t_press = now;                /* 按下沿 */
  } else {
    uint32_t dur = now - b->t_press; /* 松开沿：定档上报 */
    int is_long = dur >= LONGPRESS_MS;
    pl_button(&g_pl, b->key, is_long ? "long" : "short");
    /* 裁决在 daemon；日志按约定口径打：yes 长按 / no 任意 = deny */
    const char *sem = (b->key[0] == 'n' || is_long) ? "deny" : "allow";
    ESP_LOGI(TAG, "%s %s→%s（%lums）", b->label, is_long ? "长按" : "短按", sem, (unsigned long)dur);
    /* 空闲态长按 YES = 语音输入（授权等待时的长按是拒绝手势，voice_armed 拦下） */
    if (is_long && b->key[0] == 'y' && voice_armed()) {
      ESP_LOGI(TAG, "长按 YES → 语音录音触发");
      mic_request();
    }
  }
}

/* ---------------- 主循环 ---------------- */

static void petlink_task(void *arg)
{
  (void)arg;

  /* UART0 = 控制台 + PetLink 同流（CH343 桥到 USB-C）。日志与协议帧交错，
   * daemon 对坏行静默丢弃——与协议核心的容错设计互为犄角 */
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 1024, 0, NULL, 0));
  s_tx_mu = xSemaphoreCreateMutex();

  /* 设备标识：p4-<MAC 后 3 字节 hex>（协议 §3.1） */
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BASE);
  char dev_id[16];
  snprintf(dev_id, sizeof(dev_id), "p4-%02x%02x%02x", mac[3], mac[4], mac[5]);

  pl_cbs_t cbs = { io_send, ui_on_state, ui_on_hello_ok, io_uptime };
  pl_init(&g_pl, &cbs, NULL, dev_id, "0.4.2");

  vTaskDelay(pdMS_TO_TICKS(100)); /* 桥接芯片无枚举等待，留个稳定裕量 */
  pl_start(&g_pl, io_uptime(NULL));
  g_mic_ok = mic_init(&g_pl); /* 语音输入：录音任务 + audio 帧上行句柄 */
  ESP_LOGI(TAG, "PetLink v%d 已启动 device=%s", PL_VER, dev_id);

  uint8_t rxbuf[512];
  bool was_live = false;
  for (;;) {
    int n = uart_read_bytes(UART_NUM_0, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(20));
    uint32_t now = io_uptime(NULL);
    if (n > 0) pl_feed(&g_pl, (const char *)rxbuf, (size_t)n, now);
    btn_poll(&g_yes, now);
    btn_poll(&g_no, now);
    pl_poll(&g_pl, now);
    /* 掉线沿：helloed 翻 false 时把 UI 推回 CONNECTING（petlink.c 零改动） */
    bool live = g_pl.helloed;
    if (was_live && !live) {
      pet_ui_snap_t drop = {
        .state = PL_ST_CONNECTING,
        .link_live = false,
      };
      snprintf(drop.session, sizeof(drop.session), "%s", g_last_sess);
      drop.text[0] = '\0'; /* 掉线清标签：胶囊下方回退显示"正在连接" */
      pet_ui_push(&drop);
      ESP_LOGW(TAG, "链路掉线 → UI 回 CONNECTING");
    }
    was_live = live;
  }
}

void app_main(void)
{
#if CONFIG_PETLINK_UI_STAGE == 0
  /* v0.1 无屏模式：一键回退保险 */
  xTaskCreate(petlink_task, "petlink", 4096, NULL, 5, NULL);
#elif CONFIG_PETLINK_UI_STAGE == 1
  /* B1 点亮验证：硬件彩条，不起协议任务 */
  esp_lcd_panel_handle_t panel = disp_dsi_init();
  disp_dsi_color_bars(panel);
  ESP_LOGI(TAG, "STAGE=1 彩条模式——不跑协议；看屏确认后回 STAGE 2/3");
#elif CONFIG_PETLINK_UI_STAGE == 2
  /* B2 LVGL 静态页：不起协议任务 */
  esp_lcd_panel_handle_t panel = disp_dsi_init();
  pet_ui_mount(panel);
  ESP_LOGI(TAG, "STAGE=2 静态页模式——不跑协议；看屏确认后回 STAGE 3");
#else
  /* 正式模式：屏 + 协议（默认） */
  btn_init(&g_yes, (gpio_num_t)CONFIG_PETLINK_GPIO_YES);
  btn_init(&g_no, (gpio_num_t)CONFIG_PETLINK_GPIO_NO);
  esp_lcd_panel_handle_t panel = disp_dsi_init();
  pet_ui_mount(panel);
  xTaskCreate(petlink_task, "petlink", 6144, NULL, 5, NULL); /* 6KB：petlink 协议 + 语音触发接线 */
#endif
}
