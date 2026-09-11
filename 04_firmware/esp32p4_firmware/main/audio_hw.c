/**
 * 板载音频硬件装配实现（接口契约见 audio_hw.h）。
 *
 * 硬件事实（微雪 ESP32-P4-WIFI6 wiki「I2S Audio Demo」核实）：
 *   I²C：ES8311 @0x18，SCL=GPIO8/SDA=GPIO7（板上自带上拉）
 *   I2S：MCLK=13 / SCLK=12 / LRCK=10（P4 做 master）
 *        DIN=GPIO11（codec ASDOUT → P4，录音）
 *        DOUT=GPIO9（P4 → codec DSDIN，放音）
 *   PA ：NS4150 PA_Ctrl=GPIO53，高电平使能
 *   引脚为板载固定布线，硬编码于下方（同 DSI lanes 的处理方式）。
 */
#include "audio_hw.h"

#include <stddef.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_codec_dev_defaults.h" /* ctrl/data 工厂 + es8311_codec_new */

static const char *TAG = "audio_hw";

#define AUD_I2C_SCL    8
#define AUD_I2C_SDA    7
#define AUD_I2S_MCLK   13 /* P4 → codec 主时钟 */
#define AUD_I2S_SCLK   12
#define AUD_I2S_LRCK   10
#define AUD_I2S_DIN    11 /* codec ASDOUT → P4 数据入（录音） */
#define AUD_I2S_DOUT   9  /* P4 → codec DSDIN 数据出（放音） */
#define AUD_PA_PIN     53 /* NS4150 PA_Ctrl，高使能 */

/* 双端共享的链路（装配一次常驻；slot/时钟随各方向 open 自行重配） */
static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_in;
static esp_codec_dev_handle_t s_out;
static bool s_tryed; /* 装配只试一次：探不到 codec 就永久降级，不刷屏 */

static esp_err_t audio_hw_assemble(void)
{
  /* 1. I²C 总线：板上有上拉（wiki），不开内部上拉 */
  i2c_master_bus_config_t bus_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = AUD_I2C_SDA,
    .scl_io_num = AUD_I2C_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = false,
  };
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
  if (err != ESP_OK) return err;

  /* codec 在不在？I²C 无应答直接退（地址用 7 位形式，组件 API 用 8 位） */
  err = i2c_master_probe(s_i2c_bus, ES8311_CODEC_DEFAULT_ADDR >> 1, 200);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C 上探不到 ES8311（0x%02x @ SCL=%d/SDA=%d）",
             ES8311_CODEC_DEFAULT_ADDR >> 1, AUD_I2C_SCL, AUD_I2C_SDA);
    return err;
  }

  /* 2. I2S STD duplex 通道：TX+RX 同一次申请挂同一控制器（引脚物理共享，
   * 分两次申请会落到不同控制器打架）。slot/时钟由 esp_codec_dev_open
   * 按 (16k,16bit,mono) 自行重配——录音与放音同采样率，互不踩脚 */
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 240; /* 8×240×4B ≈ 7.7KB DMA 环 */
  err = i2s_new_channel(&cc, &s_tx, &s_rx);
  if (err != ESP_OK) return err;

  i2s_std_config_t std = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = AUD_I2S_MCLK,
      .bclk = AUD_I2S_SCLK,
      .ws = AUD_I2S_LRCK,
      .din = AUD_I2S_DIN,
      .dout = AUD_I2S_DOUT,
      .invert_flags = { 0 },
    },
  };
  err = i2s_channel_init_std_mode(s_tx, &std);
  if (err != ESP_OK) return err;
  err = i2s_channel_init_std_mode(s_rx, &std);
  if (err != ESP_OK) return err;

  /* 3. 装配：I2C 控制口 + I2S 数据口（同一 data_if 喂 TX+RX 两个方向，
   * 组件按 port 维度的互斥锁串行化重配——esp-box 同款模式） */
  audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
    .port = I2C_NUM_0,
    .addr = ES8311_CODEC_DEFAULT_ADDR, /* 8 位形式（组件内部 >>1） */
    .bus_handle = s_i2c_bus,
    .clock_speed_hz = 0,               /* 0 = 默认 100kHz */
  };
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
  if (!ctrl_if) return ESP_FAIL;

  audio_codec_i2s_cfg_t i2s_data_cfg = {
    .port = I2C_NUM_0, /* 仅作 keeper 互斥键 */
    .rx_handle = s_rx,
    .tx_handle = s_tx,
    .clk_src = 0,
  };
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
  if (!data_if) return ESP_FAIL;

  /* 4. 一颗 codec 两个方向都开：mic 的 ADC 走 IN 设备、spk 的 DAC 走
   * OUT 设备；PA 挂在 codec 上，OUT 设备 enable/close 自动拉 */
  es8311_codec_cfg_t es_cfg = {
    .ctrl_if = ctrl_if,
    .gpio_if = audio_codec_new_gpio(),
    .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH, /* 录音 + 放音 */
    .pa_pin = AUD_PA_PIN,                       /* NS4150，高使能 */
    .pa_reverted = false,
    .master_mode = false, /* P4 是 I2S master，codec 做 slave */
    .use_mclk = true,
    .digital_mic = false, /* 板载模拟麦 */
    .no_dac_ref = true,   /* 单声道，右槽留空 */
    .mclk_div = 256,
    /* 硬件增益补偿：不填时组件按“PA 5V/DAC 3.3V”默认压 -4.36dB，
     * 本板 NS4150 与 codec 同 3.3V 供电，pa_voltage 填实测值即抵消 */
    .hw_gain = {
      .pa_voltage = 3.3,
      .codec_dac_voltage = 3.3,
      .pa_gain = 0.0,
    },
  };
  const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
  if (!codec_if) return ESP_FAIL;

  esp_codec_dev_cfg_t in_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_IN,
    .codec_if = codec_if,
    .data_if = data_if,
  };
  s_in = esp_codec_dev_new(&in_cfg);
  if (!s_in) return ESP_FAIL;

  esp_codec_dev_cfg_t out_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    .codec_if = codec_if,
    .data_if = data_if,
  };
  s_out = esp_codec_dev_new(&out_cfg);
  return s_out ? ESP_OK : ESP_FAIL;
}

bool audio_hw_init(void)
{
  if (s_tryed) return audio_hw_ready();
  s_tryed = true;
  esp_err_t err = audio_hw_assemble();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "音频硬件装配失败：%s（录音/放音均降级，不影响主链路）",
             esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "ES8311 就绪（ADC+DAC）：I2C 0x%02x @ SCL=%d/SDA=%d，I2S MCLK=%d SCLK=%d LRCK=%d DIN=%d DOUT=%d，PA=%d",
           ES8311_CODEC_DEFAULT_ADDR >> 1, AUD_I2C_SCL, AUD_I2C_SDA,
           AUD_I2S_MCLK, AUD_I2S_SCLK, AUD_I2S_LRCK, AUD_I2S_DIN, AUD_I2S_DOUT,
           AUD_PA_PIN);
  return true;
}

bool audio_hw_ready(void) { return s_in != NULL && s_out != NULL; }

esp_codec_dev_handle_t audio_hw_in(void) { return s_in; }

esp_codec_dev_handle_t audio_hw_out(void) { return s_out; }
