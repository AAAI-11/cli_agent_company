/**
 * oled_board_test v1.0 —— ESP32-P4-WIFI6 主控板体检程序（F01 需求变式）。
 *
 * 在 MIPI 屏/结构设计动工之前，用一块 SSD1306 OLED 把整板关键通路过一遍：
 *   ① 供电/CPU/Flash/PSRAM（启动日志 + HW INFO 屏）
 *   ② I2C 总线（上电扫描：0x18=板载 ES8311、0x3C=OLED）
 *   ③ OLED 面板（全亮/棋盘坏点目检）
 *   ④ USB-Serial-JTAG（烧录 + 日志 + PetLink 协议同一条流）
 *   ⑤ PetLink 真协议链路：daemon 广播的真实 Claude 状态直接上屏（F01 变式）
 *   ⑥ YES 实体键：短按 <600ms / 长按 ≥600ms（与正式固件同语义）
 *
 * 数据通路与 esp32p4_firmware 完全一致（复用同一份 petlink.c/cJSON）；
 * 无 daemon 时 DEMO 轮播五态兜底——不接电脑也能看动画。
 *
 * 单任务 20ms 循环（同正式固件）：读 CDC → 喂协议 → 消抖按键 → 心跳/判活
 * → 100ms 一帧刷屏。零中断、零竞态。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "oled_ui.h"
#include "petlink.h"
#include "ssd1306.h"

#define FW_VER    "1.0.0"
#define LOOP_MS   20
#define FRAME_MS  100    /* 刷屏节奏（10fps，动画相位同源） */
#define DEMO_MS   2800   /* DEMO 模式每态停留 */
#define RETRY_MS  2000   /* OLED 未应答时的重试周期 */

/* Kconfig 布尔项未开启时宏不定义（ESP-IDF 惯例），统一折算成 0/1 */
#ifdef CONFIG_OLEDT_OLED_FLIP
#define OLEDT_FLIP 1
#else
#define OLEDT_FLIP 0
#endif

static const char *TAG = "boardtest";

/* petlink 与 oui 的状态序必须一致（可整体强转） */
_Static_assert((int)PL_ST_CONNECTING == (int)OUI_ST_CONNECTING &&
               (int)PL_ST_THINKING == (int)OUI_ST_THINKING &&
               (int)PL_ST_WORKING == (int)OUI_ST_WORKING &&
               (int)PL_ST_WAITING_CONFIRM == (int)OUI_ST_WAIT_CONFIRM &&
               (int)PL_ST_DONE == (int)OUI_ST_DONE &&
               (int)PL_ST_ERROR == (int)OUI_ST_ERROR,
               "pl_status_t 与 oui_status_t 枚举顺序不同步");

/* ---------------- 状态（UI 快照，on_state 回调里更新） ---------------- */

static pl_t g_pl;
static oui_status_t g_state = OUI_ST_CONNECTING;
static char g_session[12];
static char g_label[40]; /* cJSON 串在回调返回后即释放——必须拷贝 */

/* ---------------- DEMO 轮播：借 pl_feed 喂合成 state 帧 ---------------- */

static bool g_demo_run = true; /* 长按 YES 切换暂停 */
static int g_demo_idx;
static uint32_t g_last_demo_ms;

static const char *K_DEMO_STATUS[5] = {
  "thinking", "working", "waiting_confirm", "done", "error",
};
static const char *K_DEMO_LABEL[5] = {
  "PLANNING..", "RUN TOOL", "NEED YES ?", "TURN DONE", "SOMETHING WRONG",
};

static void demo_step(uint32_t now)
{
  char frame[128];
  snprintf(frame, sizeof(frame),
           "{\"ver\":1,\"type\":\"state\",\"session\":\"demo\","
           "\"agent\":\"claude-code\",\"status\":\"%s\",\"text\":\"%s\"}",
           K_DEMO_STATUS[g_demo_idx], K_DEMO_LABEL[g_demo_idx]);
  /* 演示帧不算链路活性（协议 §3.3 判活只认对端流量）：喂完还原判活计时 */
  uint32_t keep_rx = g_pl.last_rx_ms;
  pl_feed(&g_pl, frame, strlen(frame), now);
  g_pl.last_rx_ms = keep_rx;

  g_demo_idx = (g_demo_idx + 1) % 5;
  g_last_demo_ms = now;
}

/* ---------------- 协议核心的 IO / UI 绑定 ---------------- */

static void io_send(const char *line, void *ctx)
{
  (void)ctx;
  usb_serial_jtag_write_bytes((const uint8_t *)line, strlen(line), pdMS_TO_TICKS(50));
  usb_serial_jtag_write_bytes((const uint8_t *)"\n", 1, pdMS_TO_TICKS(50));
}

static uint32_t io_uptime(void *ctx)
{
  (void)ctx;
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ui_on_state(pl_status_t st, const char *session, const char *text, void *ctx)
{
  (void)ctx;
  g_state = (oui_status_t)st;
  snprintf(g_session, sizeof(g_session), "%s", session ? session : "");
  snprintf(g_label, sizeof(g_label), "%s", text ? text : "");
  ESP_LOGI(TAG, "[ui] %s  session=%s  text=%s",
           pl_status_name(st), g_session, g_label);
}

static void ui_on_hello_ok(const char *daemon_ver, void *ctx)
{
  (void)ctx;
  ESP_LOGI(TAG, "[ui] 握手完成 daemon=%s —— 等真实 Claude 状态", daemon_ver);
}

/* ---------------- YES 按键：消抖 + 短/长按（与正式固件同实现） ---------------- */

#define DEBOUNCE_MS  20
#define LONGPRESS_MS 600

typedef struct {
  gpio_num_t gpio;
  int stable, last_raw;
  uint32_t t_raw, t_press;
} btn_t;

static btn_t g_yes = {.stable = 1, .last_raw = 1};

static void btn_init(gpio_num_t gpio)
{
  g_yes.gpio = gpio;
  gpio_reset_pin(gpio);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
}

static void btn_poll(btn_t *b, uint32_t now)
{
  int raw = gpio_get_level(b->gpio);
  if (raw != b->last_raw) { b->last_raw = raw; b->t_raw = now; return; }
  if (now - b->t_raw < DEBOUNCE_MS) return;
  if (raw == b->stable) return;

  b->stable = raw;
  if (raw == 0) {
    b->t_press = now;
  } else {
    uint32_t dur = now - b->t_press;
    if (dur >= LONGPRESS_MS) {
      g_demo_run = !g_demo_run; /* 长按：DEMO 暂停/恢复（顺带 deny 上报） */
      g_last_demo_ms = now;
      ESP_LOGI(TAG, "YES 长按（%lums）→ deny / DEMO=%d",
               (unsigned long)dur, g_demo_run);
    } else {
      demo_step(now); /* 短按：立刻切下一态（顺带 allow 上报） */
      ESP_LOGI(TAG, "YES 短按（%lums）→ allow / 手动切态", (unsigned long)dur);
    }
    pl_button(&g_pl, "yes", dur >= LONGPRESS_MS ? "long" : "short");
  }
}

/* ---------------- 上电自检序列 ---------------- */

/** 单步闪屏 + 停留（OLED 不在时只打日志，序列照走——USB 侧可完整判读） */
static void show(int delay_ms, void (*draw)(void), const char *what)
{
  if (oled_hal_ready()) {
    draw();
    oled_hal_flush(oui_fb);
  }
  ESP_LOGI(TAG, "自检: %s", what);
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static uint8_t s_scan_addrs[16];
static int s_scan_n;

static void draw_splash(void)  { oui_splash(FW_VER, g_pl.device_id); }
static void draw_pix0(void)    { oui_pixel_test(0); }
static void draw_pix1(void)    { oui_pixel_test(1); }
static void draw_scan(void)    { oui_i2c_scan(s_scan_addrs, s_scan_n); }
static void draw_hwinfo(void);

static uint32_t s_flash_mb, s_psram_mb, s_heap_kb;
static void draw_hwinfo(void)
{
  oui_hw_info(s_flash_mb, s_psram_mb, s_heap_kb, esp_get_idf_version());
}
static void draw_pix2(void) { oui_pixel_test(2); }

/* ---------------- 主任务 ---------------- */

static void boardtest_task(void *arg)
{
  (void)arg;
  usb_serial_jtag_driver_config_t usj_cfg = {
    .rx_buffer_size = 1024,
    .tx_buffer_size = 1024,
  };
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_cfg));

  /* 设备标识：p4t-<MAC 后 3 字节>（t=test，与正式固件 p4- 区分，防同插冲突） */
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BASE);
  char dev_id[16];
  snprintf(dev_id, sizeof(dev_id), "p4t-%02x%02x%02x", mac[3], mac[4], mac[5]);

  pl_cbs_t cbs = {io_send, ui_on_state, ui_on_hello_ok, io_uptime};
  pl_init(&g_pl, &cbs, NULL, dev_id, FW_VER);

  /* ---- 上电自检序列 ---- */
  ESP_ERROR_CHECK(oled_hal_init(CONFIG_OLEDT_I2C_SDA, CONFIG_OLEDT_I2C_SCL));

  if (oled_hal_attach(CONFIG_OLEDT_I2C_ADDR, CONFIG_OLEDT_I2C_CLK_HZ,
                      OLEDT_FLIP) == ESP_OK)
    ESP_LOGI(TAG, "SSD1306 就绪 @0x%02X", CONFIG_OLEDT_I2C_ADDR);
  else
    ESP_LOGE(TAG, "SSD1306 无应答 @0x%02X —— 检查 VCC/GND/SDA(GPIO%d)/SCL(GPIO%d)",
             CONFIG_OLEDT_I2C_ADDR, CONFIG_OLEDT_I2C_SDA, CONFIG_OLEDT_I2C_SCL);

  show(1500, draw_splash, "开机闪屏");
  show(700, draw_pix0, "像素全亮（目检坏点）");
  show(700, draw_pix1, "棋盘格（目检极性/残影）");
  show(300, draw_pix2, "清屏");

  s_scan_n = oled_hal_scan(s_scan_addrs, sizeof(s_scan_addrs));
  for (int i = 0; i < s_scan_n; i++)
    ESP_LOGI(TAG, "I2C 发现 0x%02X", s_scan_addrs[i]);
  if (s_scan_n == 0) ESP_LOGE(TAG, "I2C 总线无任何应答（板载 ES8311 0x18 都没有 → 查总线/供电）");
  show(2000, draw_scan, "I2C 扫描结果");

  uint32_t flash_b = 0;
  esp_flash_get_size(NULL, &flash_b);
  s_flash_mb = flash_b >> 20;
  s_psram_mb = (uint32_t)(esp_psram_get_size() >> 20);
  s_heap_kb = esp_get_free_heap_size() >> 10;
  ESP_LOGI(TAG, "FLASH %luM  PSRAM %luM  HEAP %luK  IDF %s",
           (unsigned long)s_flash_mb, (unsigned long)s_psram_mb,
           (unsigned long)s_heap_kb, esp_get_idf_version());
  show(2000, draw_hwinfo, "硬件信息");

  /* ---- PetLink 上线 ---- */
  vTaskDelay(pdMS_TO_TICKS(100)); /* 等 host 侧 CDC 就绪 */
  pl_start(&g_pl, io_uptime(NULL));
  g_last_demo_ms = io_uptime(NULL);
  ESP_LOGI(TAG, "PetLink v%d 已启动 device=%s（无 daemon 时 %lus 后进入 DEMO）",
           PL_VER, dev_id, 3ul);

  /* ---- 主循环 ---- */
  uint8_t rxbuf[512];
  uint32_t last_frame = 0, last_retry = 0, phase = 0;
  for (;;) {
    int n = usb_serial_jtag_read_bytes(rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(LOOP_MS));
    uint32_t now = io_uptime(NULL);
    if (n > 0) pl_feed(&g_pl, (const char *)rxbuf, (size_t)n, now);
    btn_poll(&g_yes, now);
    pl_poll(&g_pl, now);

    /* OLED 不在：每 2s 重挂一次（接线修好后自动恢复，不用重烧） */
    if (!oled_hal_ready() && now - last_retry >= RETRY_MS) {
      last_retry = now;
      if (oled_hal_attach(CONFIG_OLEDT_I2C_ADDR, CONFIG_OLEDT_I2C_CLK_HZ,
                          OLEDT_FLIP) == ESP_OK) {
        ESP_LOGI(TAG, "SSD1306 重挂成功——屏恢复");
        show(800, draw_splash, "恢复闪屏");
      }
    }

    /* DEMO 兜底：没握手且 demo 开着 → 轮播五态 */
    if (!g_pl.helloed && g_demo_run && now - g_last_demo_ms >= DEMO_MS)
      demo_step(now);

    /* 10fps 刷屏（动画相位与信息行同拍） */
    if (now - last_frame >= FRAME_MS) {
      last_frame = now;
      phase++;
      oui_disp_t d = {
        .state = g_state,
        .phase = phase,
        .uptime_s = now / 1000,
        .link_live = g_pl.helloed,
        .demo_run = g_demo_run && !g_pl.helloed,
        .session = g_session,
        .label = g_label,
      };
      oui_status(&d);
      oled_hal_flush(oui_fb);
    }
  }
}

void app_main(void)
{
  btn_init((gpio_num_t)CONFIG_OLEDT_GPIO_YES);
  xTaskCreate(boardtest_task, "boardtest", 8192, NULL, 5, NULL);
}
