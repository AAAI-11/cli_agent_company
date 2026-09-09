/**
 * PetLink v1 设备端协议核心（纯 C，无 IDF 依赖，可在宿主机单测）。
 * 协议规范见 docs/protocol.md —— 本文件是其在固件侧的权威实现。
 *
 * 设计原则：
 *  - 核心不碰硬件、不知道时间流逝：字节进（pl_feed）、字节出（send_line 回调）、
 *    时间由外部喂（pl_poll 的 now_ms / uptime_ms 回调）——保证完全可测。
 *  - 前向兼容：收到未知 type / 未知字段 / 坏 JSON 一律静默忽略（协议 §1）。
 *  - 防乒乓：只应答不带 reply 的 heartbeat，应答自身带 reply:true（协议 §3.3）。
 */
#ifndef PETLINK_H
#define PETLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_VER            1
#define PL_MAX_LINE       4096  /* 单帧上限（协议 §4） */
#define PL_HEARTBEAT_MS   5000  /* 探测周期 */
#define PL_DEAD_MS        90000 /* 判活窗口（协议 §3.3） */
#define PL_AUDIO_CHUNK    1800  /* 语音帧单块 PCM 字节：b64 后 2400B，整帧 ~2.5KB < 4KB 上限 */
#define PL_MAX_BUBBLES    6     /* 多会话气泡上限（daemon focus.js 同值，协议 §3.4） */

/* 五态词汇 + 握手前初始态（协议 §3.4，与 daemon/虚拟设备三方统一） */
typedef enum {
  PL_ST_CONNECTING = 0, /* 未握手，UI 可显示"连接中" */
  PL_ST_THINKING,
  PL_ST_WORKING,
  PL_ST_WAITING_CONFIRM,
  PL_ST_DONE,
  PL_ST_ERROR,
} pl_status_t;

const char *pl_status_name(pl_status_t st);

/* F02R 多会话气泡：state 帧可选 bubbles 数组的一项（daemon 侧 focus.js 组装，
 * 焦点会话带 focus=true；数组按最新到最旧排序，最多 PL_MAX_BUBBLES 个） */
typedef struct {
  pl_status_t st;
  bool focus;
} pl_bubble_t;

typedef struct {
  /** 核心产出一条待发送的 NDJSON 行（不含 \n），传输层负责写出 */
  void (*send_line)(const char *line, void *ctx);
  /** daemon 状态广播（协议 §3.4）；text/bubbles 可能为 NULL/0（帧未携带） */
  void (*on_state)(pl_status_t st, const char *session, const char *text,
                   const pl_bubble_t *bubbles, uint8_t bubble_n, void *ctx);
  /** 握手完成；daemon_ver 为对端版本串 */
  void (*on_hello_ok)(const char *daemon_ver, void *ctx);
  /** 本机运行时长（uptime_ms 字段用） */
  uint32_t (*uptime_ms)(void *ctx);
} pl_cbs_t;

typedef struct {
  pl_cbs_t cbs;
  void *ctx;
  char device_id[24]; /* p4-<MAC后6hex>，协议 §3.1 */
  char fw[16];

  bool helloed;
  uint32_t last_rx_ms;   /* 最近一次收到任何合法帧 */
  uint32_t last_hb_ms;   /* 最近一次发出探测 */
  uint32_t last_hello_ms;/* 未握手时的 hello 重试节流 */

  char buf[PL_MAX_LINE * 2]; /* NDJSON 分帧缓冲（与 daemon LineFramer 同策略） */
  size_t len;

  pl_bubble_t bubbles[PL_MAX_BUBBLES]; /* 最近一帧 state 的多会话快照 */
  uint8_t bubble_n;

  char out[256]; /* 组包复用缓冲（控制帧 <200B；协议任务专用） */
  char out_audio[PL_MAX_LINE]; /* 语音帧组包缓冲（mic 任务专用——两任务
                                  并发出站各用各的缓冲，UART 写出由传输层串行化） */
} pl_t;

/**
 * device_id / fw 必须是模块内部生成的受控串（MAC hex / 版本号），
 * 不做 JSON 转义——不得包含引号或控制字符。
 */
void pl_init(pl_t *pl, const pl_cbs_t *cbs, void *ctx,
             const char *device_id, const char *fw);
/** 传输通道就绪后调用：发出首条 hello（协议 §2 设备先自报） */
void pl_start(pl_t *pl, uint32_t now_ms);
/** 字节流入口（USB CDC / 测试注入），内含分帧与容错；now_ms 用于判活计时 */
void pl_feed(pl_t *pl, const char *bytes, size_t n, uint32_t now_ms);
/** 周期调用：心跳到期探测、判活窗口超时重握手 */
void pl_poll(pl_t *pl, uint32_t now_ms);
/** 按键上报（协议 §3.5）：key="yes"/"btn2"/"btn3"，event="short"/"long" */
void pl_button(pl_t *pl, const char *key, const char *event);

/**
 * 语音音频帧（协议 §3.7）：单块上行。pcm 为 16bit 小端单声道裸数据；
 * end=true 发收尾帧（此时忽略 pcm）。id 为本次录音的实例标识（"a1"、"a2"…）。
 * 注意：本函数写 pl->out_audio，供 mic 任务并发调用；send_line 的串行化由传输层负责。
 */
void pl_audio(pl_t *pl, const char *id, unsigned seq, bool end, unsigned rate,
              const uint8_t *pcm, size_t n);
/** 整段录音一次发完：按 PL_AUDIO_CHUNK 分块 + 收尾帧（mic 任务的便捷入口） */
void pl_audio_buffer(pl_t *pl, const char *id, unsigned rate, const uint8_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* PETLINK_H */
