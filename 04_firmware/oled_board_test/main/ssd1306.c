/**
 * SSD1306 硬件层实现。接口语义见 ssd1306.h。
 */
#include "ssd1306.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* 整帧刷新前的地址窗口命令（列 0..127，页 0..7） */
static const uint8_t K_ADDR_WIN[6] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};

static esp_err_t cmd(const uint8_t *buf, size_t n)
{
  return i2c_master_transmit(s_dev, buf, n, -1);
}

esp_err_t oled_hal_init(int sda, int scl)
{
  i2c_master_bus_config_t cfg = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = -1, /* 自动分配端口 */
    .scl_io_num = scl,
    .sda_io_num = sda,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = false, /* 板载已有上拉 */
  };
  return i2c_new_master_bus(&cfg, &s_bus);
}

int oled_hal_scan(uint8_t *addrs, int max)
{
  int n = 0;
  for (int a = 0x08; a <= 0x77 && n < max; a++) {
    if (i2c_master_probe(s_bus, a, 20) == ESP_OK) addrs[n++] = (uint8_t)a;
  }
  return n;
}

esp_err_t oled_hal_attach(int addr, int hz, int flip)
{
  s_ready = false;
  i2c_device_config_t dc = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = addr,
    .scl_speed_hz = hz,
  };
  esp_err_t err = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
  if (err != ESP_OK) return err;

  /* 数据手册通用初始化（128×64、内部升压、水平寻址），flip 时上下镜像 */
  uint8_t init[] = {
    0x00,       /* 控制字节：后续均为命令 */
    0xAE,       /* display off */
    0x20, 0x00, /* 水平寻址模式（整帧连写） */
    0xD5, 0x80, /* 时钟分频 */
    0xA8, 0x3F, /* 复用率 1/64 */
    0xD3, 0x00, /* 显示偏移 0 */
    0x40,       /* 起始行 0 */
    0x8D, 0x14, /* 电荷泵开 */
    flip ? 0xA0 : 0xA1, /* 段重映射 */
    flip ? 0xC0 : 0xC8, /* COM 扫描方向 */
    0xDA, 0x12, /* COM 引脚配置（128×64） */
    0x81, 0xCF, /* 对比度 */
    0xD9, 0xF1, /* 预充电 */
    0xDB, 0x40, /* VCOMH */
    0xA4,       /* 显示跟随 RAM */
    0xA6,       /* 正常极性 */
    0xAF,       /* display on */
  };
  err = cmd(init, sizeof(init));
  if (err != ESP_OK) {
    i2c_master_bus_rm_device(s_dev);
    ESP_LOGE(TAG, "SSD1306 初始化失败 addr=0x%02X err=%d（检查接线/地址）", addr, err);
    return err;
  }
  s_ready = true;
  return ESP_OK;
}

bool oled_hal_ready(void) { return s_ready; }

esp_err_t oled_hal_flush(const uint8_t *fb)
{
  if (!s_ready) return ESP_ERR_INVALID_STATE;

  static uint8_t tx[1 + SSD1306_FB_SIZE]; /* +1 数据控制字节 */
  tx[0] = 0x40;

  esp_err_t err = cmd(K_ADDR_WIN, sizeof(K_ADDR_WIN));
  if (err != ESP_OK) return err;
  memcpy(tx + 1, fb, SSD1306_FB_SIZE);
  return i2c_master_transmit(s_dev, tx, sizeof(tx), -1);
}
