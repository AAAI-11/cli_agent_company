/**
 * 板载麦克风录音：语音输入的固件侧（协议 §3.7 的数据源）。
 *
 * 触发：专用 VOICE 实体键按住说话（PTT，v0.8 三键改版）——按下
 *       mic_start()、松开 mic_stop()；门控在 main.c voice_armed()
 *       （空闲态 + 已握手 + 不在授权等待 + 不忙）。
 * 流程：按住期间持续采集 16kHz 16bit 单声道 PCM → 松手（或到达上限）
 *       整段经 petlink audio 帧分块上行 → daemon whisper 转写注入 tmux。
 */
#ifndef MIC_H
#define MIC_H

#include <stdbool.h>

#include "petlink.h"

/** 协议任务起来后调用一次（录入 pl 句柄供 audio 帧上行）。false=不可用 */
bool mic_init(pl_t *pl);

/** 按下 VOICE 键：开始录音（非阻塞；录音任务会把后续流程接管完） */
void mic_start(void);

/** 松开 VOICE 键：停止录音并把已有数据整段上行（未在录音时为空操作） */
void mic_stop(void);

/** 录音+上行进行中（voice_armed 的忙判定） */
bool mic_busy(void);

#endif /* MIC_H */
