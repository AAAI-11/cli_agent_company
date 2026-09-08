/**
 * 板载麦克风录音实现：ES8311 codec + I2S STD RX（独立任务，按需启停）。
 *
 * 硬件事实（微雪 ESP32-P4-WIFI6 wiki「I2S Audio Demo」2026-09-08 核实）：
 *   板载模拟麦 → ES8311 codec（I²C 地址 0x18，SCL=GPIO8/SDA=GPIO7）
 *   → ASDOUT=GPIO11；P4 做 I2S master：MCLK=13/SCLK=12/LRCK=10。
 *   引脚为板载固定布线，硬编码于下方（同 DSI lanes 的处理方式）。
 *   历史教训：v0.4.0/0.4.1 误信网上资料把 GPIO23/24 当 PDM 直连麦，
 *   左右槽翻转+增益扫遍全零——那两个脚上根本没有麦克风。
 *
 * 音频栈：espressif/esp_codec_dev（esp-box / P4 EV 板现行官方栈）。
 * open(16k/16bit/mono) 自动把 I2S 通道重配成 STD MONO+LEFT 槽，
 * esp_codec_dev_read 直接吐 mono PCM——与 pl_audio/whisper 口径无缝。
 *
 * 并发模型：
 *   - 协议任务（main.c）：UART RX 喂协议、按键轮询——录音期间照常跑
 *   - mic 任务：本文件独占 I2S/I2C/codec 与 pl_audio（写 pl->out_audio；
 *     UART TX 由 main.c io_send 的互斥锁串行化）
 *
 * 失败兜底：I2C 探不到 ES8311 / 通道建立失败只记日志、任务继续等下一
 * 次请求——语音输入是增强功能，绝不拖垮按键授权主链路。
 */
#include "mic.h"

#if CONFIG_PETLINK_MIC_ENABLED

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h" /* ctrl/data 工厂 + es8311_codec_new */

#include "pet_ui.h"

static const char *TAG = "mic";

#define MIC_RATE_HZ 16000 /* whisper 的舒适采样率：16k/16bit/单声道 */

/* 微雪 P4-WIFI6 板载 ES8311 布线（wiki 核实，勿改——改板才改） */
#define MIC_I2C_SCL    8
#define MIC_I2C_SDA    7
#define MIC_I2S_MCLK   13 /* P4 → codec 主时钟 */
#define MIC_I2S_SCLK   12
#define MIC_I2S_LRCK   10
#define MIC_I2S_DIN    11 /* codec ASDOUT → P4 数据入 */
/* GPIO9(DSDIN)/GPIO53(PA_Ctrl) 属放音链路，录音不碰 */

static pl_t *s_pl;
static TaskHandle_t s_task;
static volatile bool s_busy;
static int16_t *s_pcm; /* 录音缓冲（懒分配，PSRAM，常驻复用） */
static unsigned s_seq; /* 录音实例计数（audio 帧 id） */

/* esp_codec_dev 装配件（懒初始化后常驻；open/close 随每次录音起落） */
static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_dev;

bool mic_busy(void) { return s_busy; }

void mic_request(void)
{
  if (s_task && !s_busy) xTaskNotifyGive(s_task);
}

static esp_err_t codec_rx_init(void)
{
  /* 1. I²C 总线：板上有上拉（wiki），不开内部上拉 */
  i2c_master_bus_config_t bus_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = MIC_I2C_SDA,
    .scl_io_num = MIC_I2C_SCL,
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
             ES8311_CODEC_DEFAULT_ADDR >> 1, MIC_I2C_SCL, MIC_I2C_SDA);
    return err;
  }

  /* 2. I2S STD RX 通道：这里只定引脚 + DMA 环；slot/时钟由
   * esp_codec_dev_open 按 (16k,16bit,mono) 自行重配 */
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 240; /* 8×240×4B ≈ 7.7KB DMA 环——6s 录音按 2KB 粒度收割 */
  err = i2s_new_channel(&cc, NULL, &s_rx);
  if (err != ESP_OK) return err;

  i2s_std_config_t std = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE_HZ),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = MIC_I2S_MCLK,
      .bclk = MIC_I2S_SCLK,
      .ws = MIC_I2S_LRCK,
      .din = MIC_I2S_DIN,
      .dout = -1, /* 不放音 */
      .invert_flags = { 0 },
    },
  };
  err = i2s_channel_init_std_mode(s_rx, &std);
  if (err != ESP_OK) return err;

  /* 3. 装配：I2C 控制口 + I2S 数据口 + ES8311 + 输入设备 */
  audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
    .port = I2C_NUM_0,
    .addr = ES8311_CODEC_DEFAULT_ADDR, /* 8 位形式（组件内部 >>1） */
    .bus_handle = s_i2c_bus,
    .clock_speed_hz = 0,               /* 0 = 默认 100kHz */
  };
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
  if (!ctrl_if) return ESP_FAIL;

  audio_codec_i2s_cfg_t i2s_data_cfg = {
    .port = I2C_NUM_0, /* 仅作 keeper 互斥键，实际用 rx_handle */
    .rx_handle = s_rx,
    .tx_handle = NULL,
    .clk_src = 0,
  };
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
  if (!data_if) return ESP_FAIL;

  es8311_codec_cfg_t es_cfg = {
    .ctrl_if = ctrl_if,
    .gpio_if = audio_codec_new_gpio(),
    .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC, /* 只录不放 */
    .pa_pin = -1,                              /* PA 属放音链路，跳过 */
    .master_mode = false,                      /* P4 是 I2S master */
    .use_mclk = true,                          /* P4 经 GPIO13 供 MCLK */
    .digital_mic = false,                      /* 板载模拟麦 */
    .no_dac_ref = true,                        /* 单声道录制，右槽留空 */
    .mclk_div = 256,
  };
  const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
  if (!codec_if) return ESP_FAIL;

  esp_codec_dev_cfg_t dev_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_IN,
    .codec_if = codec_if,
    .data_if = data_if,
  };
  s_dev = esp_codec_dev_new(&dev_cfg);
  return s_dev ? ESP_OK : ESP_FAIL;
}

/* UI 反馈：录音中/已发送——本地快照，daemon 真实状态一到即被覆盖 */
static void push_local(pl_status_t st, const char *text)
{
  pet_ui_snap_t snap = { .state = st, .link_live = s_pl->helloed };
  snprintf(snap.text, sizeof(snap.text), "%s", text);
  pet_ui_push(&snap);
}

static void mic_task(void *arg)
{
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* 等触发，不占 CPU */

    size_t want = (size_t)MIC_RATE_HZ * 2 * CONFIG_PETLINK_MIC_RECORD_MS / 1000;
    if (!s_pcm) s_pcm = heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (!s_pcm) {
      ESP_LOGE(TAG, "PSRAM 录音缓冲分配失败（%uB）——跳过本段", (unsigned)want);
      continue;
    }
    if (!s_dev) { /* 懒初始化：codec 探不到时避免每次录音都刷屏 */
      esp_err_t err = codec_rx_init();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 录音链初始化失败：%s", esp_err_to_name(err));
        continue;
      }
      ESP_LOGI(TAG, "ES8311 就绪：I2C 0x%02x @ SCL=%d/SDA=%d，I2S MCLK=%d SCLK=%d LRCK=%d DIN=%d",
               ES8311_CODEC_DEFAULT_ADDR >> 1, MIC_I2C_SCL, MIC_I2C_SDA,
               MIC_I2S_MCLK, MIC_I2S_SCLK, MIC_I2S_LRCK, MIC_I2S_DIN);
    }

    s_busy = true;
    push_local(PL_ST_WORKING, "录音中");

    esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 1, /* 驱动自动转 STD MONO+LEFT 槽 */
      .sample_rate = MIC_RATE_HZ,
    };
    size_t got = 0;
    int ret = esp_codec_dev_open(s_dev, &fs);
    if (ret == 0) {
      esp_codec_dev_set_in_gain(s_dev, (float)CONFIG_PETLINK_MIC_GAIN_DB);
      ESP_LOGI(TAG, "录音开始：%dms @%dHz gain=%ddB",
               CONFIG_PETLINK_MIC_RECORD_MS, MIC_RATE_HZ,
               CONFIG_PETLINK_MIC_GAIN_DB);
      while (got < want) {
        size_t chunk = (want - got > 2048) ? 2048 : want - got;
        ret = esp_codec_dev_read(s_dev, (uint8_t *)s_pcm + got, chunk);
        if (ret != 0) break; /* 通道无数据/超时由组件兜底退场 */
        got += chunk;
      }
      esp_codec_dev_close(s_dev);
    }
    if (ret != 0) {
      ESP_LOGW(TAG, "采集中断（ret=%d）：已收 %uB / %uB", ret,
               (unsigned)got, (unsigned)want);
    }

    char id[12];
    snprintf(id, sizeof(id), "a%u", ++s_seq);
    pl_audio_buffer(s_pl, id, MIC_RATE_HZ, (const uint8_t *)s_pcm, got);
    push_local(PL_ST_DONE, "语音已发送");
    ESP_LOGI(TAG, "录音上行完成 id=%s：%uB / %uB", id, (unsigned)got, (unsigned)want);
    s_busy = false;
  }
}

bool mic_init(pl_t *pl)
{
  s_pl = pl;
  BaseType_t ok = xTaskCreate(mic_task, "mic", 6144, NULL, 4, &s_task);
  if (ok != pdPASS) {
    s_task = NULL;
    ESP_LOGE(TAG, "录音任务创建失败——语音输入不可用（不影响主链路）");
    return false;
  }
  ESP_LOGI(TAG, "语音输入就绪：长按 YES（非授权等待时）录音 %dms，ES8311 gain=%ddB",
           CONFIG_PETLINK_MIC_RECORD_MS, CONFIG_PETLINK_MIC_GAIN_DB);
  return true;
}

#else /* !CONFIG_PETLINK_MIC_ENABLED */

bool mic_init(pl_t *pl) { (void)pl; return false; }
void mic_request(void) {}
bool mic_busy(void) { return false; }

#endif /* CONFIG_PETLINK_MIC_ENABLED */
