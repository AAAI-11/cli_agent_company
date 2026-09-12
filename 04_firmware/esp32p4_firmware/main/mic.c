/**
 * 板载麦克风录音实现：ES8311 ADC + I2S STD RX（独立任务，按需启停）。
 *
 * 硬件装配（I2C/I2S/codec 双方向设备）自 v3.3 起抽到 audio_hw.c——
 * 提示音（F16）与录音共享同一条 duplex I2S 通道和同一颗 ES8311。
 *
 * 历史教训：v0.4.0/0.4.1 误信网上资料把 GPIO23/24 当 PDM 直连麦，
 * 左右槽翻转+增益扫遍全零——那两个脚上根本没有麦克风。
 *
 * 录音模型（v0.8 按住说话）：VOICE 键按下 mic_start / 松开 mic_stop。
 * 一段录音在三种情况下结束：松手（正常）、按住超过上限
 * CONFIG_PETLINK_MIC_MAX_MS（自动截断并照常上行）、快于 500ms 的
 * 误触（本地丢弃，不空跑转写）。
 *
 * 音频栈：espressif/esp_codec_dev（esp-box / P4 EV 板现行官方栈）。
 * open(16k/16bit/mono) 自动把 I2S 通道重配成 STD MONO+LEFT 槽，
 * esp_codec_dev_read 直接吐 mono PCM——与 pl_audio/whisper 口径无缝。
 *
 * 并发模型：
 *   - 协议任务（main.c）：UART RX 喂协议、按键轮询——录音期间照常跑
 *   - mic 任务：本文件独占录音设备（写 pl->out_audio；UART TX 由
 *     main.c io_send 的互斥锁串行化）；录音优先于提示音（spk.c 遵守）
 *
 * 失败兜底：ES8311 探不到只记日志、任务继续等下一次请求——语音输入
 * 是增强功能，绝不拖垮按键授权主链路。
 */
#include "mic.h"

#if CONFIG_PETLINK_MIC_ENABLED

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_codec_dev.h"

#include "audio_hw.h"
#include "pet_ui.h"

static const char *TAG = "mic";

#define MIC_RATE_HZ 16000 /* whisper 的舒适采样率：16k/16bit/单声道 */
#define MIC_MIN_MS 500    /* 松手快于这个时长视为误触，整段丢弃 */
#define MIC_BIT_START 1u  /* 按下通知位 */
#define MIC_BIT_STOP 2u   /* 松开通知位 */

static pl_t *s_pl;
static TaskHandle_t s_task;
static volatile bool s_busy;
static int16_t *s_pcm; /* 录音缓冲（懒分配，PSRAM，常驻复用） */
static unsigned s_seq; /* 录音实例计数（audio 帧 id） */

bool mic_busy(void) { return s_busy; }

void mic_start(void)
{
  if (s_task) xTaskNotify(s_task, MIC_BIT_START, eSetBits);
}

void mic_stop(void)
{
  if (s_task) xTaskNotify(s_task, MIC_BIT_STOP, eSetBits);
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
    /* 等按下沿（START 位）；STOP 位只在采集循环里消费 */
    uint32_t bits = 0;
    xTaskNotifyWait(MIC_BIT_START, MIC_BIT_START, &bits, portMAX_DELAY);
    if (!(bits & MIC_BIT_START)) continue;

    size_t cap = (size_t)MIC_RATE_HZ * 2 * CONFIG_PETLINK_MIC_MAX_MS / 1000;
    if (!s_pcm) s_pcm = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!s_pcm) {
      ESP_LOGE(TAG, "PSRAM 录音缓冲分配失败（%uB）——跳过本段", (unsigned)cap);
      continue;
    }
    esp_codec_dev_handle_t dev = audio_hw_init() ? audio_hw_in() : NULL;
    if (!dev) continue; /* ES8311 不在位，静默降级（日志 audio_hw 已记） */

    /* 清掉录音开始前就排队的 STOP（上传间隙快速点按的松手），这种
     * 段按下时人已松手，录了也是废段，直接跳过 */
    uint32_t early = 0;
    if (xTaskNotifyWait(MIC_BIT_STOP, MIC_BIT_STOP, &early, 0) &&
        (early & MIC_BIT_STOP)) {
      ESP_LOGI(TAG, "按下时松手已排队——忽略本段");
      continue;
    }

    s_busy = true;
    push_local(PL_ST_WORKING, "录音中");

    esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 1, /* 驱动自动转 STD MONO+LEFT 槽 */
      .sample_rate = MIC_RATE_HZ,
    };
    size_t got = 0;
    int ret = esp_codec_dev_open(dev, &fs);
    if (ret == 0) {
      esp_codec_dev_set_in_gain(dev, (float)CONFIG_PETLINK_MIC_GAIN_DB);
      ESP_LOGI(TAG, "录音开始 @%dHz gain=%ddB（按住说话，上限 %dms）",
               MIC_RATE_HZ, CONFIG_PETLINK_MIC_GAIN_DB,
               CONFIG_PETLINK_MIC_MAX_MS);
      while (got < cap) {
        size_t chunk = (cap - got > 2048) ? 2048 : cap - got;
        ret = esp_codec_dev_read(dev, (uint8_t *)s_pcm + got, chunk);
        if (ret != 0) break; /* 通道无数据/超时由组件兜底退场 */
        got += chunk;
        uint32_t stop = 0; /* 每 128ms（2048B）探一次松手 */
        if (xTaskNotifyWait(MIC_BIT_STOP, MIC_BIT_STOP, &stop, 0) &&
            (stop & MIC_BIT_STOP)) {
          break;
        }
      }
      esp_codec_dev_close(dev);
    }

    const size_t min_len = (size_t)MIC_RATE_HZ * 2 * MIC_MIN_MS / 1000;
    const unsigned ms = (unsigned)(got * 1000u / (MIC_RATE_HZ * 2));
    if (ret != 0 && got == 0) {
      ESP_LOGW(TAG, "采集失败（ret=%d）——本段丢弃", ret);
    } else if (got < min_len) {
      push_local(PL_ST_DONE, "太短已忽略");
      ESP_LOGI(TAG, "录音过短（%ums < %ums）——丢弃", ms, (unsigned)MIC_MIN_MS);
    } else {
      char id[12];
      snprintf(id, sizeof(id), "a%u", ++s_seq);
      pl_audio_buffer(s_pl, id, MIC_RATE_HZ, (const uint8_t *)s_pcm, got);
      push_local(PL_ST_DONE, "语音已发送");
      ESP_LOGI(TAG, "录音上行完成 id=%s：%uB（约 %ums，上限 %dms%s）",
               id, (unsigned)got, ms, CONFIG_PETLINK_MIC_MAX_MS,
               got >= cap ? "，自动截断" : "");
    }
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
  ESP_LOGI(TAG, "语音输入就绪：VOICE 键按住说话（上限 %dms），ES8311 gain=%ddB",
           CONFIG_PETLINK_MIC_MAX_MS, CONFIG_PETLINK_MIC_GAIN_DB);
  return true;
}

#else /* !CONFIG_PETLINK_MIC_ENABLED */

bool mic_init(pl_t *pl) { (void)pl; return false; }
void mic_start(void) {}
void mic_stop(void) {}
bool mic_busy(void) { return false; }

#endif /* CONFIG_PETLINK_MIC_ENABLED */
