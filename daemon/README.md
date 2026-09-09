# companion-daemon

CLI Agent 伴侣桌面 daemon：Claude Code hooks 状态归一化 + PetLink v1 设备链路 + 语音输入输出 + 会话统计。
协议见 [`docs/protocol.md`](../docs/protocol.md)（宪法：[需求说明文档 v3.1](../01_需求与方案/需求说明文档.md)）。

## 运行（Day-1 形态）

```bash
cd daemon
npm install
npm start -- --fake        # 假状态流（无 Claude Code 时）
```

然后浏览器打开 **http://localhost:7331/device** —— 虚拟设备宠物随假状态流变脸。

| 端口 | 用途 |
|---|---|
| 7331 | HTTP：状态页 `/`、hooks `POST /state`、授权 `POST /permission`、统计 `GET /stats`、注入 `POST /inject`、虚拟设备 `/device` |
| 7332 | WebSocket：虚拟设备接入（PetLink 一帧一消息） |
| 7333 | TCP：NDJSON 调试回环 |

**USB CDC（实体设备主链路）**：daemon 启动即自动扫描串口（Espressif/WCH/FTDI/CP210x），插上即连、拔掉 2s 重扫。可用 `PETLINK_SERIAL_PATH=/dev/cu.xxx` 锁定串口、`PETLINK_BAUD` 改波特率（**默认 921600**，v0.4 起与固件 console 同步升档——语音上行 32KB/s 的带宽要求；老固件用 `PETLINK_BAUD=115200` 回退）。

macOS 串口要点（`links.js` 已内置，排障时知其所以然）：同一设备有 `cu.`（callout）/`tty.`（dialin）两个节点，serialport 的 `list()` 只报 `tty.*`——按身份选中后**换开同名 `cu.` 节点**。开 `tty.` 的写会被静默丢弃（设备只重发 hello 收不到 hello_ok，2026-09-04 复现过），且杀掉持有 `tty.` 的进程可能让 CH343 从总线消失（需拔插 USB）。

**tmux 文本注入**（演示戏剧效果件，需 `brew install tmux`）：

```bash
curl -s localhost:7331/inject/panes                       # 列出所有 pane
curl -sX POST localhost:7331/inject \
  -d '{"pane":"work:0.0","text":"继续","enter":true}'     # 像人手一样打进终端
```

## 语音输出（F12，v0.2）

忙别的时让 daemon 念出来：`waiting_confirm` / `done` / `error` 状态与 permission 放行/拒绝结果。

```bash
PETLINK_VOICE=1 npm start    # 开启（默认关——CI/深夜编码不被念叨）
```

- macOS：`say`（自动挑第一个中文语音）；Windows：PowerShell System.Speech（系统自带零依赖）；Linux：espeak-ng
- 可调：`PETLINK_VOICE_SPEAKER`（语音名）、`PETLINK_VOICE_GAP_MS`（冷却间隔，默认 3000）
- 防刷：3s 冷却 + 同文本 10s 不重念（working↔thinking 翻转不吵）

## 会话统计（F13，v0.2）

```bash
curl -s localhost:7331/stats | jq
```

计数器：hook 事件分布、五态次数与停留时长（status_ms）、permission allow/deny/none、按键（yes:short…）、语音转写打点（requests/ok/fail/chars + 最近一条转写）。daemon 生命周期内累计，重启清零——结题报告的数字来源。

## 语音输入（F14，v0.2）

设备长按 YES（非授权等待时）说一句话 → 本地 whisper 转写 → 注入 tmux pane（等价于人手键入）。

一次性准备（依赖都不入仓库，`models/` 已 gitignore）：

```bash
brew install whisper-cpp                                  # macOS；Windows 用官方预编译包 + PETLINK_WHISPER_BIN 指过去
curl -L -o models/ggml-small.bin \
  https://hf-mirror.com/ggerganov/whisper.cpp/resolve/main/ggml-small.bin   # 466MB，国内用 hf-mirror
```

- 注入目标：唯一 tmux pane 时自动选它；多 pane 设 `PETLINK_INJECT_PANE=work:0.0`
- 可调：`PETLINK_WHISPER_MODEL`（模型路径）、`PETLINK_WHISPER_LANG`（默认 zh）
- 缺依赖时 daemon 照常跑：首次收到音频才告警一次，按键授权主链路完全不受影响

## 创新点 F03 的演示闭环

```bash
# 终端 A：挂起一个授权请求（15s 内等设备按键）
curl -sX POST localhost:7331/permission -H 'Content-Type: application/json' \
  -d '{"session_id":"demo","tool":"Bash","prompt":"rm -rf /tmp/junk"}'
# 浏览器：点击虚拟设备的 YES（短按）
# → curl 返回 {"decision":"allow",...,"by":"device:virtual-1"}
```

长按 YES = 拒绝；NO 键按下即拒；不按键 = 超时 deny。

## 多会话焦点（F02R，v0.5）

多个 Claude Code 会话同时挂到同一台 daemon 时，屏幕（state 帧）永远演**焦点会话**，
其余会话以 `bubbles` 数组附在帧里（协议 §3.4，旧设备忽略即退化为单会话）：

- **waiting_confirm 无条件抢占焦点**——不错过任何一个会话的授权请求
- 焦点会话超时淘汰后回落到最新活跃会话
- **空闲态按 NO 键循环切换焦点**（有授权挂起时 NO 仍是按下即拒，语义不被偷走）
- 语音播报跟屏走：设备演的是谁，就念谁的状态

## 冒烟测试

```bash
npm run smoke    # Node WS 客户端扮演设备，全链路断言（26 项：含统计/语音/audio 重组/多会话焦点）
```

## 路线（W1）

- [ ] `hooks/install.js`：合并写入 `~/.claude/settings.json`，接真实 Claude Code（PermissionRequest HTTP hook → `POST /permission`）
- [x] `UsbCdcLink`：serialport 打开 CDC 口，与 TcpLink 同构（macOS cu./tty. 已踩平，见上）
- [x] tmux send-keys 注入 adapter（`/inject`，2026-09-04 通）
- [x] 语音输出 say 播报 + 会话统计 /stats（2026-09-07，v0.2）
- [x] 语音输入：audio 重组 + whisper 转写 + tmux 注入（2026-09-07，v0.2）
