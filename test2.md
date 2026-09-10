# v0.7.0 真机验收单（F16 状态提示音）

> 前提：终端 A daemon 在跑（`npm start`）、终端 B 已按烧录命令刷入 v0.7.0。
> 喇叭没到也能测前三步（日志层）；到货后补第四步（听声层）。

## 一、固件层（喇叭未到也能验）

### a. [ ] monitor 确认任务与装配日志

烧录后 monitor（`idf.py -p /dev/cu.usbmodem5B910634541 flash monitor`）里应看到：

```
I (xxx) spk: 状态提示音就绪：waiting_confirm/done/error 转移时播放（NS4150 + 外接喇叭）
```

> `audio_hw: ES8311 就绪（ADC+DAC）` 这行要等到**第一次触发提示音**才出现（懒初始化）。

`Ctrl+]` 退出 monitor：

```bash
curl -X POST http://127.0.0.1:7331/usb/resume
```

### b. [ ] daemon 在线后触发 done 转移

等终端 A 出现 `握手完成 → p4-e396bd (fw=0.7.0)`，然后：

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"Stop","session_id":"sess-a"}'
```

✅ 屏变 done。**此处无声属预期**（喇叭没接，NS4150 空载），日志也看不到——
monitor 和 daemon 抢 USB 口不能同开（a 步看完已退出），听声阶段声音本身就是判据。

> 想看 `audio_hw: ES8311 就绪（ADC+DAC）` / `spk: 提示音：完成` 这两行日志的替代办法：
> 暂停 daemon（`curl -X POST http://127.0.0.1:7331/usb/pause`）→ 终端 B `idf.py -p /dev/cu.usbmodem5B910634541 monitor`
> → 但此时 daemon 断开、状态帧到不了板子，得手动拔插 USB 触发重连重放才能听到一次提示音——
> 没必要折腾，跳过即可。

### c. [ ] 防吵验证：同状态重复帧不响

紧接 b 再发**一模一样**的 curl，再发一次。✅ 判定：行为无异常、屏不闪、（接了喇叭的话）不再响第二次——重放去重生效。

> 声层判据：接喇叭后 b 响一次、c 不响才算过。

## 二、声层（喇叭到货后补验）

### d. [ ] 接喇叭

8Ω 无源小喇叭（¥2 那只）接板上喇叭接口（丝印 SPK/SPEAKER，2pin；焊盘款就焊线）。
⚠️ **必须无源**——买成带功放的有源喇叭会破音。

### e. [ ] 三种音色逐个听

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"Stop","session_id":"sess-a"}'
```

✅ done：**叮-咚**（低→高上扬，轻快）

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"Notification","session_id":"sess-a","notif":"permission","message":"rm -rf /tmp/x"}'
```

✅ waiting_confirm：**叮-叮**（880Hz 双高音，急促）——这两声最要紧，它替你喊「来看一眼」

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"Notification","session_id":"sess-a","notif":"error","message":"boom"}'
```

✅ error：**嘟**（330Hz 低长音，沉）

> 注意状态要「转移进来」：先处于非 error 态再发第三条；连续发同一状态不会响（c 步已验）。
> 音量：`idf.py menuconfig` → PetLink 状态提示音 → 音量（默认 70，夜里调 30）。

### f. [ ] 录音优先验证

长按 YES 录音（F14 链路）**期间**触发一条 e 步 curl。✅ 判定：录音正常完成、提示音不响（mic 忙整段丢弃，grill 定稿）；录音结束后下一个转移恢复出声。

### g. [ ] 回归：语音输入未被改坏

mic.c 被重构过（硬件装配移到 audio_hw.c），必测：空闲态长按 YES 6s → daemon 日志照旧出现 whisper 转写（哪怕乱码幻觉也算链路通）。

## 三、收尾

- [ ] 全过 → 手册 F16 行 ⏳ 翻 ✅（真机日期）；`test2.md` 可删或归档
- [ ] git status 干净

## 附：排查速查

| 症状 | 查 |
|---|---|
| b 步屏不变 done | 终端 A 有没有 `握手完成`；`fw=0.7.0` 是否出现（旧固件） |
| monitor 无 spk 就绪日志 | 刷的是不是 v0.7.0；Kconfig 开关（menuconfig → PetLink 状态提示音） |
| 日志有「提示音：完成」但无声 | 喇叭没插好 / 买了有源喇叭 / PA 极性反——最后一种改 `audio_hw.c` `pa_reverted` 为 true 重刷 |
| 「I2C 上探不到 ES8311」 | SCL=8/SDA=7 被占？同 F14 排查（手册 §13） |
| 声音破/爆 | 正常应无爆音（5ms 包络）；确认喇叭阻抗 4~8Ω |
| 录音和提示音同时想要 | 设计如此：录音优先，提示音丢弃不排队 |
