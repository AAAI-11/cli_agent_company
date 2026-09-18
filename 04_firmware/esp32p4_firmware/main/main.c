/**
 * petlink-fw v0.9 —— ESP32-P4 固件入口（三键 + 按住说话 + 摇杆导航版）。
 *
 * 链路：UART0 @921600（板上 USB-C 经 CH343 桥接到 UART0——微雪 P4-WIFI6 的
 *       实际布线，P4 原生 USJ 未引出；daemon 的 UsbCdcLink 按 WCH VID 1a86 识别。
 *       921600 = 语音上行带宽：16kHz×16bit = 32KB/s，115200 只有 ~11.5KB/s）
 * 按键：YES / NO / VOICE 三键（GPIO menuconfig 可改），一脚接 GPIO 一脚接 GND，内部上拉；
 *       YES 任意按下松开 = 放行（不再有时长判档——长按拒绝手势 v0.8 废除）；
 *       NO 键按下即拒（不看时长，2026-09-03 真机两次没按准 600ms 的教训）；
 *       VOICE 键按住说话（PTT）：按下开始录音、松手上传（与原版哈基米机
 *        「长按唤醒语音」的用户习惯对齐）
 * 摇杆：X 轴 ADC（F17，v3.5 §4.8）——左右推动 → button 帧 prev/next，
 *       daemon 侧双向切焦点会话 + tmux 视图同步。GPIO -1 = 禁用（默认，
 *       引脚号待原理图读数补）。与语音同门控哲学：授权等待时让位
 * 语音：空闲态 VOICE 键按住 = 板载麦录音 → 松手 audio 帧上行（mic.c，协议 §3.7）；
 *       授权等待（waiting_confirm）时语音让位给按键裁决，不触发录音
 * UI  ：MIPI-DSI 480×640 + LVGL 六态宠物（v0.2 起接管 ui_on_state——快照推送，
 *       渲染全在 LVGL 任务，petlink 协议核心零改动）
 *
 * 任务模型：协议任务 20ms 一拍（读 CDC → 喂协议核心 → 消抖按键 → 摇杆采样
 * → 心跳/判活）+ mic 录音任务（事件触发）。UART TX 互斥锁保证两任务并发发帧不劈行。
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
#include "spk.h"
#include "joystick.h"

static const char *TAG = "petlink";

/* ---------------- 协议核心的 IO 绑定 ---------------- */

static pl_t g_pl;
static SemaphoreHandle_t s_tx_mu;

static void io_send(const char *line, void *ctx)
{
  (void)ctx;
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

static char g_last_sess[16] = "";
static pl_status_t g_last_status = PL_ST_CONNECTING;

static void copy_utf8_bounded(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  memcpy(dst, src ? src : "", n);
  dst[n] = '\0';
}

/* on_state 回调：agent 为 daemon 侧会话的管道标识（"codex-cli"/"claude-code"等），
 * 空/未知时用空串。pet_ui_snap_t 自动带 agent 字段供渲染层区分图标。 */
static void ui_on_state(pl_status_t st, const char *session, const char *agent,
                        const char *text,
                        const pl_bubble_t *bubbles, uint8_t bubble_n,
                        const pl_tokens_t *tokens, void *ctx)
{
  (void)ctx;
  g_last_status = st;
  spk_alert(st);
  ESP_LOGI(TAG, "[ui] %s  agent=%s  session=%s  text=%s  bubbles=%u  tokens=%s",
           pl_status_name(st), agent ? agent : "-", session ? session : "-", text ? text : "-",
           bubbles ? bubble_n : 0,
           tokens ? "y" : "-");
  pet_ui_snap_t snap = {
    .state = st,
    .link_live = g_pl.helloed,
  };
  snprintf(snap.session, sizeof(snap.session), "%s", session ? session : "");
  snprintf(g_last_sess, sizeof(g_last_sess), "%s", session ? session : "");
  snprintf(snap.agent, sizeof(snap.agent), "%s", agent ? agent : "");
  copy_utf8_bounded(snap.text, sizeof(snap.text), text);
  if (bubbles && bubble_n > PL_MAX_BUBBLES) bubble_n = PL_MAX_BUBBLES;
  snap.bubble_n = bubble_n;
  if (bubbles && bubble_n) memcpy(snap.bubbles, bubbles, bubble_n * sizeof(pl_bubble_t));
  snap.tokens_valid = tokens != NULL;
  if (tokens) snap.tokens = *tokens;
  pet_ui_push(&snap);
}

static void ui_on_hello_ok(const char *daemon_ver, void *ctx)
{
  (void)ctx;
  ESP_LOGI(TAG, "[ui] 握手完成 daemon=%s", daemon_ver);
  pet_ui_link(true);
  pet_ui_snap_t snap = {
    .state = PL_ST_DONE,   /* 无会话门面：星轨 +「待机」（与浏览器 hello_ok 口径一致） */
    .link_live = true,
    .text = "待机",
  };
  pet_ui_push(&snap);
}

/* ---------------- 实体按键：消抖 + 按下/松开沿分发 ---------------- */

#define DEBOUNCE_MS 20

typedef struct {
  gpio_num_t gpio;
  const char *label;
  int stable;
  int last_raw;
  uint32_t t_raw;
  uint32_t t_press;
  void (*on_down)(void);
  void (*on_up)(void);
} btn_t;

static bool g_mic_ok;

static void yes_up(void)
{
  pl_button(&g_pl, "yes", "short");
  ESP_LOGI(TAG, "YES→allow");
}

static void no_up(void)
{
  pl_button(&g_pl, "no", "short");
  ESP_LOGI(TAG, "NO→deny");
}

static bool voice_armed(void)
{
  return g_mic_ok && g_pl.helloed &&
         g_last_status != PL_ST_WAITING_CONFIRM && !mic_busy();
}

static void voice_down(void)
{
  if (!voice_armed()) {
    ESP_LOGI(TAG, "VOICE 按下但未武装（未连接/授权等待/录音中）——忽略");
    return;
  }
  ESP_LOGI(TAG, "VOICE 按下 → 开始录音");
  mic_start();
}

static void voice_up(void)
{
  if (mic_busy()) {
    ESP_LOGI(TAG, "VOICE 松开 → 结束录音并上传");
    mic_stop();
  }
}

/* ---------------- 摇杆导航（F17，v3.5 §4.8） ---------------- */

static joy_axis_t g_joy;
static bool g_joy_ok;

static bool joy_armed(void)
{
  return g_joy_ok && g_pl.helloed &&
         g_last_status != PL_ST_WAITING_CONFIRM && !mic_busy();
}

static void joy_dispatch(int dir)
{
  if (!joy_armed()) return;
  pl_button(&g_pl, dir < 0 ? "prev" : "next", "short");
  ESP_LOGI(TAG, "摇杆 %s", dir < 0 ? "← prev" : "next →");
}

static btn_t g_yes   = { .label = "YES",   .stable = 1, .last_raw = 1,
                         .gpio = (gpio_num_t)-1, .on_up = yes_up };
static btn_t g_no    = { .label = "NO",    .stable = 1, .last_raw = 1,
                         .gpio = (gpio_num_t)-1, .on_up = no_up };
static btn_t g_voice = { .label = "VOICE", .stable = 1, .last_raw = 1,
                         .gpio = (gpio_num_t)-1, .on_down = voice_down, .on_up = voice_up };

static void btn_init(btn_t *b, gpio_num_t gpio)
{
  b->gpio = gpio;
  gpio_reset_pin(gpio);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
}

static void btn_poll(btn_t *b, uint32_t now)
{
  int raw = gpio_get_level(b->gpio);
  if (raw != b->last_raw) {
    b->last_raw = raw;
    b->t_raw = now;
    return;
  }
  if (now - b->t_raw < DEBOUNCE_MS) return;
  if (raw == b->stable) return;

  b->stable = raw;
  if (raw == 0) {
    b->t_press = now;
    if (b->on_down) b->on_down();
  } else {
    if (b->on_up) b->on_up();
  }
}

/* ---------------- 主循环 ---------------- */

static void petlink_task(void *arg)
{
  (void)arg;

  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 1024, 0, NULL, 0));
  s_tx_mu = xSemaphoreCreateMutex();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BASE);
  char dev_id[16];
  snprintf(dev_id, sizeof(dev_id), "p4-%02x%02x%02x", mac[3], mac[4], mac[5]);

  pl_cbs_t cbs = { io_send, ui_on_state, ui_on_hello_ok, io_uptime };
  pl_init(&g_pl, &cbs, NULL, dev_id, "0.9.0");

  vTaskDelay(pdMS_TO_TICKS(100));
  pl_start(&g_pl, io_uptime(NULL));
  g_mic_ok = mic_init(&g_pl);
#if defined(CONFIG_PETLINK_GPIO_JOY_X) && CONFIG_PETLINK_GPIO_JOY_X >= 0
  joy_axis_init(&g_joy, 300, 180);
  g_joy_ok = joystick_init(&g_joy);
  if (!g_joy_ok) ESP_LOGW(TAG, "摇杆初始化失败——不影响按键授权主链路");
#endif
  if (!spk_init()) ESP_LOGW(TAG, "状态提示音不可用（编译关或任务创建失败）");
  ESP_LOGI(TAG, "PetLink v%d 已启动 device=%s", PL_VER, dev_id);

  uint8_t rxbuf[512];
  bool was_live = false;
  for (;;) {
    int n = uart_read_bytes(UART_NUM_0, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(20));
    uint32_t now = io_uptime(NULL);
    if (n > 0) pl_feed(&g_pl, (const char *)rxbuf, (size_t)n, now);
    btn_poll(&g_yes, now);
    btn_poll(&g_no, now);
    if (g_voice.gpio >= 0) btn_poll(&g_voice, now);
#if defined(CONFIG_PETLINK_GPIO_JOY_X) && CONFIG_PETLINK_GPIO_JOY_X >= 0
    if (g_joy_ok) {
      int32_t jmv = joystick_read_x();
      if (jmv != INT32_MIN) {
        int jev = joy_axis_feed(&g_joy, jmv, now);
        if (jev != 0) joy_dispatch(jev);
      }
    }
#endif
    pl_poll(&g_pl, now);
    bool live = g_pl.helloed;
    if (was_live && !live) {
      pet_ui_snap_t drop = {
        .state = PL_ST_CONNECTING,
        .link_live = false,
      };
      snprintf(drop.session, sizeof(drop.session), "%s", g_last_sess);
      drop.text[0] = '\0';
      pet_ui_push(&drop);
      ESP_LOGW(TAG, "链路掉线 → UI 回 CONNECTING");
    }
    was_live = live;
  }
}

void app_main(void)
{
#if CONFIG_PETLINK_UI_STAGE == 0
  xTaskCreate(petlink_task, "petlink", 4096, NULL, 5, NULL);
#elif CONFIG_PETLINK_UI_STAGE == 1
  esp_lcd_panel_handle_t panel = disp_dsi_init();
  disp_dsi_color_bars(panel);
  ESP_LOGI(TAG, "STAGE=1 彩条模式——不跑协议；看屏确认后回 STAGE 2/3");
#elif CONFIG_PETLINK_UI_STAGE == 2
  esp_lcd_panel_handle_t panel = disp_dsi_init();
  pet_ui_mount(panel);
  ESP_LOGI(TAG, "STAGE=2 静态页模式——不跑协议；看屏确认后回 STAGE 3");
#else
  btn_init(&g_yes, (gpio_num_t)CONFIG_PETLINK_GPIO_YES);
  btn_init(&g_no, (gpio_num_t)CONFIG_PETLINK_GPIO_NO);
  if (CONFIG_PETLINK_GPIO_VOICE >= 0)
    btn_init(&g_voice, (gpio_num_t)CONFIG_PETLINK_GPIO_VOICE);
  esp_lcd_panel_handle_t panel = disp_dsi_init();
  pet_ui_mount(panel);
  xTaskCreate(petlink_task, "petlink", 6144, NULL, 5, NULL);
#endif
}
