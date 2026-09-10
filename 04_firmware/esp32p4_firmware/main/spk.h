/**
 * 状态提示音（F16）：waiting_confirm/done/error 状态转移时，板载
 * ES8311 DAC + NS4150 功放播放正弦合成提示音（外接 8Ω 喇叭）。
 *
 * 触发集与 F12（daemon 侧 Mac TTS）一致：Mac 在身边听语音，Mac 静音/
 * 离身时板子自己会喊。同状态重复帧不响（重连重放、周期刷新不吵）。
 *
 * 并发：录音优先——mic 忙时提示音直接丢弃不排队（grill 定稿）。
 * 播放失败只记日志，绝不拖垮状态主链路。
 */
#pragma once

#include <stdbool.h>

#include "petlink.h"

/* 独立任务 + 通知触发；重复触发在播放期间到达会被合并（不排队） */
void spk_request(void);

/* 状态转移进入三态之一时由 main.c 调用（内部去重：同状态不重响） */
void spk_alert(pl_status_t st);

/* 任务创建失败/编译关闭时返回 false（main.c 只记日志） */
bool spk_init(void);

/* 正在出声（main.c 诊断用） */
bool spk_busy(void);
