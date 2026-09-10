/**
 * 状态提示音实现（接口契约见 spk.h，宪法 §4.6 / F16）。
 *
 * 音色（16k/16bit/mono 正弦合成，无素材文件）：
 *   waiting_confirm：叮-叮（880Hz ×2，急促）
 *   done           ：叮-咚（587→880Hz 上扬，轻快）
 *   error          ：嘟（330Hz 长低音，沉）
 * 每段 5ms 线性淡入淡出防爆音；音量走 ES8311 DAC 数字音量（Kconfig）。
 *
 * 与 mic.c 的并发：录音优先——播放前查 mic_busy()，忙则整段丢弃不排队
 * （grill 定稿：提示音可丢，语音输入不可丢）。
 */
#include "spk.h"

#if CONFIG_PETLINK_SPK_ENABLED

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "esp_codec_dev.h"

#include "audio_hw.h"
#include "mic.h"

static const char *TAG = "spk";

#define SPK_RATE_HZ 16000
#define SPK_ENV_MS  5 /* 每段首尾线性包络，防爆音 */

/* 一段音：正弦频率与时长；freq=0 表示静音间隔 */
typedef struct {
  unsigned freq;
  unsigned dur_ms;
} seg_t;

/* 三态音色（宪法 §4.6 定稿；触发集与 F12 播报集一致） */
static const seg_t k_alert[][3] = {
  [PL_ST_WAITING_CONFIRM] = { { 880, 90 }, { 0, 70 }, { 880, 90 } },
  [PL_ST_DONE]            = { { 587, 90 }, { 0, 40 }, { 880, 140 } },
  [PL_ST_ERROR]           = { { 330, 220 }, { 0, 0 }, { 0, 0 } },
};

static TaskHandle_t s_task;
static volatile bool s_busy;
static volatile pl_status_t s_pending = PL_ST_CONNECTING; /* 待播状态（通知携带） */
static pl_status_t s_last_seen = PL_ST_CONNECTING;        /* 去重：上一次见到的状态 */

bool spk_busy(void) { return s_busy; }

void spk_alert(pl_status_t st)
{
  /* 同状态重复帧不响：重连重放、周期刷新（F12 同款防吵规则）。
   * 只有"新进入"三态之一才算转移（error→error 不响，error→done 响） */
  if (st == s_last_seen) return;
  s_last_seen = st;
  if (st != PL_ST_WAITING_CONFIRM && st != PL_ST_DONE && st != PL_ST_ERROR) return;
  s_pending = st;
  if (s_task) xTaskNotifyGive(s_task);
}

/* 合成一段正弦到 out（含首尾包络），返回写入的样本数 */
static unsigned synth_seg(int16_t *out, unsigned cap, unsigned freq, unsigned dur_ms)
{
  const unsigned n = dur_ms * SPK_RATE_HZ / 1000;
  if (n == 0 || n > cap) return 0;
  if (freq == 0) {
    memset(out, 0, n * sizeof(int16_t)); /* 静音间隔 */
    return n;
  }
  const unsigned env = SPK_ENV_MS * SPK_RATE_HZ / 1000;
  const float w = 2.0f * (float)M_PI * (float)freq / (float)SPK_RATE_HZ;
  const float amp = 0.6f * 32767.0f;
  for (unsigned i = 0; i < n; i++) {
    float g = 1.0f; /* 梯形包络：5ms 淡入/淡出 */
    if (i < env) g = (float)i / (float)env;
    else if (i > n - 1 - env && n > env) g = (float)(n - 1 - i) / (float)env;
    out[i] = (int16_t)(sinf(w * (float)i) * amp * g);
  }
  return n;
}

static void spk_task(void *arg)
{
  (void)arg;
  static int16_t pcm[SPK_RATE_HZ / 2]; /* 500ms 上限 ≈ 16KB，内部 RAM 常驻 */
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (mic_busy()) { /* 录音优先：整段丢弃，不排队（grill 定稿） */
      ESP_LOGD(TAG, "录音中，丢弃提示音");
      continue;
    }
    esp_codec_dev_handle_t dev = audio_hw_init() ? audio_hw_out() : NULL;
    if (!dev) continue; /* ES8311 不在位，静默降级（audio_hw 已记日志） */

    const seg_t *segs = k_alert[s_pending];
    unsigned n = 0;
    for (unsigned i = 0; i < 3; i++)
      n += synth_seg(pcm + n, SPK_RATE_HZ / 2 - n, segs[i].freq, segs[i].dur_ms);

    esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 1,
      .sample_rate = SPK_RATE_HZ,
    };
    int ret = esp_codec_dev_open(dev, &fs);
    if (ret == 0) {
      s_busy = true;
      esp_codec_dev_set_out_vol(dev, (float)CONFIG_PETLINK_SPK_VOLUME);
      ESP_LOGI(TAG, "提示音：%s（%u 样本，vol=%d）",
               pl_status_name(s_pending), n, CONFIG_PETLINK_SPK_VOLUME);
      /* 16KB 一次 write 太大（DMA 环 7.7KB），2KB 分块喂（接口参数不带 const） */
      uint8_t *p = (uint8_t *)pcm;
      size_t left = n * sizeof(int16_t);
      while (left > 0 && (ret = esp_codec_dev_write(dev, p, left > 2048 ? 2048 : left)) == 0) {
        p += left > 2048 ? 2048 : left;
        left -= left > 2048 ? 2048 : left;
      }
      esp_codec_dev_close(dev); /* close 自动静音+关 PA（防爆破音时序） */
      s_busy = false;
    }
    if (ret != 0) ESP_LOGW(TAG, "提示音播放失败（ret=%d）——不影响状态主链路", ret);
  }
}

bool spk_init(void)
{
  BaseType_t ok = xTaskCreate(spk_task, "spk", 4096, NULL, 4, &s_task);
  if (ok != pdPASS) {
    s_task = NULL;
    ESP_LOGE(TAG, "提示音任务创建失败——状态提示音不可用（不影响主链路）");
    return false;
  }
  ESP_LOGI(TAG, "状态提示音就绪：waiting_confirm/done/error 转移时播放（NS4150 + 外接喇叭）");
  return true;
}

#else /* !CONFIG_PETLINK_SPK_ENABLED */

bool spk_init(void) { return false; }
void spk_alert(pl_status_t st) { (void)st; }
bool spk_busy(void) { return false; }

#endif /* CONFIG_PETLINK_SPK_ENABLED */
