/**
 * 板载音频硬件装配：ES8311 codec 的录音(ADC)/放音(DAC)共享层。
 *
 * mic.c（F14 录音）与 spk.c（F16 提示音）共用一条 duplex I2S 通道和
 * 同一颗 ES8311——MCLK/SCLK/LRCK 引脚物理上只有一套，两个方向必须
 * 挂同一个 I2S 控制器（esp_codec_dev 的 keeper 互斥按 port 串行化
 * 重配，esp-box 官方同款模式）。装配一次，两个 esp_codec_dev 各取
 * 所需（IN 读 / OUT 写），引用计数由组件维护。
 *
 * PA：NS4150 功放，PA_Ctrl=GPIO53 高电平使能，codec enable/close 自动
 * 拉高/拉低（es8311 驱动内置，防爆破音时序）。喇叭接口外接 8Ω 无源
 * 小喇叭（4~8Ω 均可，3W D 类对口）。
 *
 * 失败兜底：I2C 探不到 ES8311 / 通道建立失败只记一次日志、返回 false，
 * 上层（mic/spk）各自降级，绝不拖垮按键授权主链路。
 */
#pragma once

#include <stdbool.h>
#include "esp_codec_dev.h"

/* 懒初始化（首次使用时装配；成功后常驻）。失败返回 false 且不再重试刷屏 */
bool audio_hw_init(void);

/* 装配是否成功（失败时 mic/spk 各自静默降级） */
bool audio_hw_ready(void);

/* 录音设备（esp_codec_dev_read 口）；未就绪返回 NULL */
esp_codec_dev_handle_t audio_hw_in(void);

/* 放音设备（esp_codec_dev_write 口，PA 随 enable 自动门控）；未就绪返回 NULL */
esp_codec_dev_handle_t audio_hw_out(void);
