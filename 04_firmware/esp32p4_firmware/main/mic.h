/**
 * 板载麦克风录音（PDM → I2S RX）：语音输入的固件侧（协议 §3.7 的数据源）。
 *
 * 触发：空闲态长按 YES（waiting_confirm 时长按 YES 是拒绝手势，不抢——
 *       门控在 main.c voice_armed()）。
 * 流程：mic_request() 通知录音任务 → PDM RX 采 N 毫秒 16kHz 16bit 单声道
 *       PCM → petlink audio 帧分块上行 → daemon whisper 转写注入 tmux。
 * 引脚：微雪 P4-WIFI6 板载 MSM261S PDM 麦（网上资料 GPIO23=DIN/24=CLK，
 *       以真机实测为准——Kconfig 可改，menuconfig 里调）。
 */
#ifndef MIC_H
#define MIC_H

#include <stdbool.h>

#include "petlink.h"

/** 协议任务起来后调用一次（录入 pl 句柄供 audio 帧上行）。false=不可用 */
bool mic_init(pl_t *pl);

/** 请求录一段（非阻塞；未初始化/上一段还没发完时静默忽略） */
void mic_request(void);

/** 录音+上行进行中（voice_armed 的忙判定） */
bool mic_busy(void);

#endif /* MIC_H */
