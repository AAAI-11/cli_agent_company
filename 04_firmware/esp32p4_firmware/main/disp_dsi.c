/**
 * MIPI-DSI 屏幕点亮（WLK2802MIPI-15P：2.8″ 480×640 圆 IPS，ST7701S）。
 * 参数来源=卖家资料《WLK2802MIPI-15P V1 屏幕资料》（register 值属硬件接口
 * 事实，经卖家通道取得）+ 卖家 ESP32-P4 验证组件包：
 *   - FPC 仅 1 对数据 lane（DSI_DN/DP），无 RST 脚 → 1 lane 总线 + 软复位
 *   - 背光 5 串白光 LED 直接吃 VCC 3.3V，模块/主板均无驱动芯片
 *     （2026-09-04 I2C 实测 0x45 无应答，仅板载 ES8311@0x18）→ 常亮无代码
 *   - Normally Black：未初始化=黑脸（此前黑屏的外观侧印证）
 *   - 卖家 P4 验证值：25MHz 像素钟，hsync 30/10/30，vsync 50/8/50，
 *     lane 速率 700Mbps，init 序列 39 条
 * 此前 2 lane@1250 + 48MHz + 150/150/50 时序对 1-lane 屏物理不可收——黑屏根因。
 */
#include "disp_dsi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"

static const char *TAG = "disp_dsi";

/**
 * WLK2802MIPI-15P 初始化序列（卖家组件 esp_lcd_st7701.c 默认表逐条抄录，
 * 与卖家 TXT《ST7701S+28-480640_INIT》核对一致——C2 取组件值 07 14）。
 * 组件自带的 espressif 默认表是另一款屏（C0/gamma 值不同），不能用；
 * 旧"最小两条序列"假设 OTP 自启动——本屏无 OTP，2026-09-04 真机证伪。
 */
static const st7701_lcd_init_cmd_t k_wlk_2802_init_cmds[] = {
  /* Command2 BK0：显示行数/porch/极性/驱动方式/gamma */
  {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
  {0xEF, (uint8_t[]){0x08}, 1, 0},
  {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
  {0xC0, (uint8_t[]){0x4F, 0x00}, 2, 0},   /* 显示行设置 480 line */
  {0xC1, (uint8_t[]){0x10, 0x0C}, 2, 0},   /* porch 控制 */
  {0xC2, (uint8_t[]){0x07, 0x14}, 2, 0},   /* 反转/帧率 */
  {0xCC, (uint8_t[]){0x10}, 1, 0},
  {0xB0, (uint8_t[]){0x0A, 0x18, 0x1E, 0x12, 0x16, 0x0C, 0x0E, 0x0D,
                     0x0C, 0x29, 0x06, 0x14, 0x13, 0x29, 0x33, 0x1C}, 16, 0},
  {0xB1, (uint8_t[]){0x0A, 0x19, 0x21, 0x0A, 0x0C, 0x00, 0x0C, 0x03,
                     0x03, 0x23, 0x01, 0x0E, 0x0C, 0x27, 0x2B, 0x1C}, 16, 0},
  /* Command2 BK1：电源/GIP */
  {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
  {0xB0, (uint8_t[]){0x5D}, 1, 0},         /* Vop 幅度 */
  {0xB1, (uint8_t[]){0x61}, 1, 0},         /* VCOM 幅度 */
  {0xB2, (uint8_t[]){0x84}, 1, 0},         /* VGH */
  {0xB3, (uint8_t[]){0x80}, 1, 0},
  {0xB5, (uint8_t[]){0x4D}, 1, 0},         /* VGL */
  {0xB7, (uint8_t[]){0x85}, 1, 0},
  {0xB8, (uint8_t[]){0x20}, 1, 0},
  {0xC1, (uint8_t[]){0x78}, 1, 0},
  {0xC2, (uint8_t[]){0x78}, 1, 0},
  {0xD0, (uint8_t[]){0x88}, 1, 0},         /* MIPI 设置 */
  {0xE0, (uint8_t[]){0x00, 0x00, 0x02}, 3, 0},
  {0xE1, (uint8_t[]){0x06, 0xA0, 0x08, 0xA0, 0x05, 0xA0, 0x07, 0xA0,
                     0x00, 0x44, 0x44}, 11, 0},
  {0xE2, (uint8_t[]){0x20, 0x20, 0x44, 0x44, 0x96, 0xA0, 0x00, 0x00,
                     0x96, 0xA0, 0x00, 0x00}, 12, 0},
  {0xE3, (uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4, 0},
  {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
  {0xE5, (uint8_t[]){0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0, 0xA0,
                     0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0}, 16, 0},
  {0xE6, (uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4, 0},
  {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
  {0xE8, (uint8_t[]){0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0, 0xA0,
                     0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0}, 16, 0},
  {0xE9, (uint8_t[]){0x36, 0x00}, 2, 0},
  {0xEB, (uint8_t[]){0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0},
  {0xED, (uint8_t[]){0xFF, 0x45, 0x67, 0xFA, 0x01, 0x2B, 0xCF, 0xFF,
                     0xFF, 0xFC, 0xB2, 0x10, 0xAF, 0x76, 0x54, 0xFF}, 16, 0},
  {0xEF, (uint8_t[]){0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 6, 0},
  /* 收尾：关 Command2 → 退睡眠（等 120ms）→ 开显示（等 20ms） */
  {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
  {0x11, (uint8_t[]){0x00}, 1, 120},
  {0x29, (uint8_t[]){0x00}, 1, 20},
};

esp_lcd_panel_handle_t disp_dsi_init(void)
{
  /* 1) DSI PHY 上电：LDO 通道/电压 Kconfig 可调（默认 3/2500） */
  esp_ldo_channel_handle_t ldo_phy = NULL;
  esp_ldo_channel_config_t ldo_cfg = {
    .chan_id = CONFIG_PETLINK_DSI_LDO_CHAN,
    .voltage_mv = CONFIG_PETLINK_DSI_LDO_MV,
  };
  ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_phy));
  ESP_LOGI(TAG, "DSI PHY 已上电（LDO ch%d @%dmV）",
           CONFIG_PETLINK_DSI_LDO_CHAN, CONFIG_PETLINK_DSI_LDO_MV);

  /* 2) DSI 总线：1 lane @ 700Mbps（FPC 只有一对数据线；速率=卖家验证值） */
  esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
  esp_lcd_dsi_bus_config_t bus_cfg = {
    .bus_id = 0,
    .num_data_lanes = 1,
    .lane_bit_rate_mbps = 700,
  };
  ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

  /* 3) DBI 命令通道（发 init 序列用） */
  esp_lcd_dbi_io_config_t dbi_cfg = {
    .virtual_channel = 0,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
  };
  esp_lcd_panel_io_handle_t dbi_io = NULL;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

  /* 4) DPI 视频时序：卖家 P4 验证值 25MHz、30/10/30、50/8/50（必须 static） */
  static const esp_lcd_dpi_panel_config_t dpi_cfg = {
    .virtual_channel = 0,
    .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
    .dpi_clock_freq_mhz = 25,
    .in_color_format = LCD_COLOR_FMT_RGB888,
    .out_color_format = LCD_COLOR_FMT_RGB888,
    .num_fbs = 1,
    .video_timing = {
      .h_size = 480,
      .v_size = 640,
      .hsync_back_porch = 30,
      .hsync_pulse_width = 10,
      .hsync_front_porch = 30,
      .vsync_back_porch = 50,
      .vsync_pulse_width = 8,
      .vsync_front_porch = 50,
    },
  };

  /* 5) ST7701 面板：卖家 init 表（见上方注释） */
  st7701_vendor_config_t vendor_cfg = {
    .mipi_config = {
      .dsi_bus = dsi_bus,
      .dpi_config = &dpi_cfg,
    },
    .flags = {
      .use_mipi_interface = 1,
    },
    .init_cmds = k_wlk_2802_init_cmds,
    .init_cmds_size = sizeof(k_wlk_2802_init_cmds) / sizeof(st7701_lcd_init_cmd_t),
  };
  esp_lcd_panel_dev_config_t dev_cfg = {
    .reset_gpio_num = CONFIG_PETLINK_LCD_RST_GPIO,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 24,
    .vendor_config = &vendor_cfg,
  };

  esp_lcd_panel_handle_t panel = NULL;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(dbi_io, &dev_cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  /* FPC 无 RST 脚→组件走 SWRESET 只等 20ms；卖家序列要求复位后等 120ms */
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_LOGI(TAG, "ST7701 480x640 init OK（WLK2802MIPI-15P 卖家序列，1 lane）");

  return panel;
}

void disp_dsi_color_bars(esp_lcd_panel_handle_t panel)
{
  /* DSI 控制器硬件测试图——完全绕过 framebuffer/LVGL，验证链路最短路径 */
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
  ESP_LOGI(TAG, "彩条已输出（硬件 pattern）——屏亮=时序/init 全对");
}
