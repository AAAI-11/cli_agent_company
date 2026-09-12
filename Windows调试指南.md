# Windows 调试指南（移植清单）

> 目的：队友想在 Windows 机器上跑这套项目的 daemon / 虚拟设备 / 烧录，照此清单逐项做。
> Mac 是主力开发机（全部真机验证在此完成），Windows 是**二级调试环境**——
> 能跑的范围和 Mac 不完全一致，先看 §0 的能力对照，再决定从哪层入手。

## 0. 先看能力对照（Windows 上什么能跑、什么不能）

| 层 | Windows 状态 | 说明 |
|---|---|---|
| 浏览器虚拟设备 | ✅ 直接跑 | 纯浏览器 + WebSocket，无平台依赖 |
| daemon HTTP/WS/TCP 面 | ✅ 直接跑 | `npm start` 即可，hooks/授权/统计/气泡/token 全部可用 |
| daemon USB 链路（连板子） | ✅ 大概率能跑 | VID 过滤逻辑跨平台；需装 CH343 驱动 + 确认口名（§2） |
| 固件烧录 | ✅ 能 | ESP-IDF Windows 版装一遍（§4） |
| F12 语音播报 | ✅ 已有分支 | voice.js 内置 Windows SAPI 路径，零依赖（§3） |
| F14 语音注入 | ❌ 暂不支持 | tmux Windows 没有，注入目标定位需重写（§5） |
| 宿主单测 / 冒烟 | ✅ 直接跑 | 纯 Node，无平台依赖（§1） |

一句话：**想看板子状态、演示授权按键、听提示音——Windows 全套可用；
想用"对着板子说话→注入 Claude"——留在 Mac 上**。

## 1. 基础安装（Node + 代码 + 自检）

```bash
node --version
```

要求 Node ≥ 18（daemon 用了内置 fetch）。没有就去 https://nodejs.org 装 LTS。

```bash
git clone https://github.com/AAAI-11/cli_agent_company.git
cd cli_agent_company/daemon
npm install
```

自检（不碰硬件即可跑，32 项全过 = daemon 核心没坏）：

```bash
npm run smoke
```

> 冒烟里的 ASR 断言预期是"fail 打点"（whisper 没装时），这是**通过**不是失败——
> 断言的是"缺依赖时正确打点降级"，不是转写成功。

## 2. USB 连板子（CH343 驱动 + 口名确认）

板子的 USB-C 走 WCH CH343 桥接（VID 1a86），Windows 需要驱动：

1. 下载 WCH 串口驱动：https://www.wch.cn/downloads/CH343SER_ZIP.html
   （解压后跑 `SETUP.EXE`；Win11 一般即插即用，插上先看设备管理器有没有端口）
2. 插板子 → 设备管理器 → "端口(COM 和 LPT)" → 找到类似 `USB-SERIAL CH343 (COM5)` 的条目，
   记下 COM 号
3. daemon 的口扫描**不需要改代码**：它按 VID 过滤（`links.js` 的 KNOWN_VIDS 含 1a86），
   Windows 上 serialport 枚举出 `COM5` 这类路径，能直接命中
4. 想固定口名（防插拔换号），设环境变量再启动：

```bash
set PETLINK_SERIAL_PATH=COM5
npm start
```

✅ 预期 daemon 日志：`UsbCdcLink 连接 COM5（wch.cn）@921600` + 握手完成 → p4-e396bd

❌ 连上但没握手 → 波特率对不对（921600，老固件 115200）；
❌ 打开失败 "Access denied" → 另一个程序占着 COM 口（串口助手/VSCode/另一个 daemon），关掉它

**已知平台差异**（links.js 里的处理是 Mac 专属，Windows 不受影响）：
Mac 上 serialport 只报 `tty.*` 要换 `cu.*` 节点；Windows 的 COM 口没有这个二义性，
代码里的 `process.platform === 'darwin'` 分支会自动跳过，无需改动。

## 3. F12 语音播报（Windows 自带，零安装）

voice.js 已写好 Windows 分支（powershell + System.Speech），直接可用：

```bash
set PETLINK_VOICE=1
npm start
```

✅ 预期：waiting_confirm/done/error 与授权结果用中文 SAPI 语音念出来。
❌ 没声音 → 系统音量/静音；或系统未装中文语音包（设置 → 时间和语言 → 语言 →
中文 → 语音，添加后重试）。

## 4. 固件烧录（ESP-IDF Windows 版）

装 ESP-IDF v6.1 Windows 安装器：https://dl.espressif.com/dl/esp-idf/
（选 v6.1，与 Mac 侧 sdkconfig 一致；装完开始菜单里有 "ESP-IDF 6.1 PowerShell"）

用官方 PowerShell（自带环境变量）：

```powershell
cd cli_agent_company\04_firmware\esp32p4_firmware
idf.py -p COM5 flash monitor
```

⚠️ 三个 Mac 侧约定同样适用：
- 烧录前若 daemon 在跑，先让它松口（`curl -X POST http://127.0.0.1:7331/usb/pause`），
  否则 COM 口被占
- monitor 退出是 `Ctrl+]`（Windows PowerShell 一样）
- 固件目录必须全 ASCII（`04_firmware`）——Windows 中文用户名路径同样会炸，
  clone 到 `C:\dev\` 之类的短路径最稳

❌ `requires chip revision [v3.1-v3.99]` → sdkconfig.defaults 已修（REV_MIN_100），
确认 sdkconfig 没被重生成覆盖。

## 5. F14 语音注入：Windows 上为什么不能跑 & 替代方案

Mac 侧注入链路是 **tmux send-keys**（`daemon/src/inject.js`）：whisper 转写完，
把文字"打进"指定 tmux 窗格的 claude 输入框。Windows 没有 tmux，这条路是断的。

- 录音上行、whisper 转写、统计打点这些**在 Windows 全部正常**（断了只是最后注入一步）
- daemon 日志会写 `[voice-in] ... 无转写结果` 或注入失败——是预期的
- 真要在 Windows 用，替代路径（**未实现，仅备忘**）：claude 跑在 Windows Terminal 里，
  用 PowerShell SendKeys / nircmd 模拟键入——但"哪台窗口哪个会话"的定位比 tmux 弱，
  多会话注入（F02R 的组合场景）基本不可靠，这就是当初选 tmux 的原因

## 6. hooks 安装（Windows 同一条命令）

hooks 装进 `%USERPROFILE%\.claude\settings.json`（路径自适应，无需改）：

```bash
cd daemon
node src/install-hooks.js
```

✅ 预期输出与 Mac 一致（重挂 8 个状态事件 + PermissionRequest）。
新开 claude 会话干活，daemon 日志出现真实 session 事件即通。

## 7. 速查：Mac vs Windows 差异总表

| 项 | Mac | Windows |
|---|---|---|
| 串口名 | `/dev/cu.usbmodem*` | `COM5`（设备管理器查） |
| 串口驱动 | 系统自带 | WCH CH343SER 驱动 |
| 语音播报 | `say` | SAPI（powershell，内置） |
| 语音注入 | tmux send-keys | ❌ 无等价物（备忘：SendKeys） |
| ESP-IDF | `~/.espressif/v6.1` | 官方 Windows 安装器 + ESP-IDF PowerShell |
| daemon 启动 | `PETLINK_VOICE=1 npm start` | `set PETLINK_VOICE=1` 然后 `npm start` |
| 指定串口 | `PETLINK_SERIAL_PATH=/dev/cu... npm start` | `set PETLINK_SERIAL_PATH=COM5` 后 `npm start` |

## 8. 遇到问题

排障主文档：《功能与排障手册.md》第三部分（速查表 + 破案记录，平台无关的故障都在那）。
Windows 专属问题优先对照本文 §2/§4 的 ❌ 行；实在卡住，把 daemon 日志 + 设备管理器
截图发到群里。
