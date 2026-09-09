# PetLink 协议规范 v1

**状态**：基线（Day-1 定稿，可演进）
**适用**：固件（ESP32-P4）、桌面 daemon（Node）、虚拟设备（浏览器）三方共同实现依据
**原则**：极简、前向兼容、传输无关。**本协议为项目自研，命名与格式与任何第三方实现无重合。**

---

## 1. 总则

- 消息 = 一个 JSON 对象（UTF-8）。
- **字节流传输**（USB CDC / TCP）：NDJSON 帧——一个 JSON 对象占一行，以 `\n` 结尾。
- **WebSocket 传输**（浏览器虚拟设备）：一个 message frame 携带一个 JSON 对象（浏览器无法原生 TCP，帧语义与 NDJSON 等价）。
- 所有消息必带两个顶层字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `ver` | int | 协议版本，本版恒为 `1` |
| `type` | string | 消息类型，见 §3 |

- **前向兼容**：接收方遇到未知 `type` 或未知字段，必须静默忽略，不得断连。
- **版本协商**：连接建立后首条消息必须为 `hello`。`ver` 不支持时回 `error{code:"version_mismatch"}` 并关闭连接。

## 2. 角色与连接生命周期

```
daemon（桌面，常驻）                     device（实体固件 / 虚拟设备）
      │◄──────────── hello ───────────────│  连接后设备先自报
      │──────────── hello_ok ────────────►│
      │◄──────────── heartbeat ───────────│  心跳，≥5s 一条，双向均可发
      │──────────── state ───────────────►│  daemon 单向广播状态
      │◄──────────── button ──────────────│  设备上报按键
      │──── error（可选，出错方发）───────►│
```

- 一台 daemon 可挂多台 device（一实一虚即此能力）；`state` 向所有已握手设备广播。
- 心跳超时 90s 判定链路死亡（见 §3.3 判活窗口），由设备侧重连。

## 3. 消息定义

### 3.1 hello（device → daemon，首条）

```json
{"ver":1,"type":"hello","device":"virtual-1","fw":"0.1.0","hw":"browser","caps":["button"]}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `device` | ✓ | 设备唯一标识；实体设备建议 `p4-<MAC后6位hex>`，虚拟设备 `virtual-<n>` |
| `fw` | ✓ | 固件/实现版本，语义化字符串 |
| `hw` | ✗ | 硬件形态：`p4-board` / `browser` / 其他 |
| `caps` | ✗ | 能力数组，v1 定义 `"button"`（实体按键）、`"mic"`（板载录音，见 §3.7）；未知项忽略 |

### 3.2 hello_ok（daemon → device）

```json
{"ver":1,"type":"hello_ok","app":"companion-daemon","daemon_ver":"0.1.0","echo":"virtual-1"}
```

`echo` 回显 `device`，供设备确认握手对象。

### 3.3 heartbeat（保活探测）

```json
{"ver":1,"type":"heartbeat","uptime_ms":123456}          // 探测
{"ver":1,"type":"heartbeat","uptime_ms":123457,"reply":true} // 应答
```

- `uptime_ms` 为发送方运行时长。
- **防乒乓规则**：收到不带 `reply` 的 heartbeat，回一条带 `"reply":true` 的应答；收到带 `reply:true` 的 heartbeat，**不得再回**——否则双方互答会形成消息风暴（实测教训）。
- 判活窗口 90s（浏览器后台标签页会把定时器节流到 ~60s 一次，窗口必须覆盖）。

### 3.4 state（daemon → device，广播）

```json
{"ver":1,"type":"state","session":"a1b2","agent":"claude-code","status":"thinking","text":"梳理需求",
 "bubbles":[{"s":"a1b2","st":"thinking","f":1},{"s":"c3d4","st":"working","f":0}]}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `session` | ✓ | Agent 会话标识（Claude Code session_id 截断或透传） |
| `agent` | ✓ | `claude-code`（v1 唯一；为多 Agent 预留） |
| `status` | ✓ | 五态之一，见下表 |
| `text` | ✗ | ≤32 字符短标签，设备可截断显示 |
| `bubbles` | ✗ | 多会话快照（F02R）：存活会话按最新到最旧排序，最多 6 项；`s`=会话标识前 8 字符（仅显示用）、`st`=五态、`f`=1 表示**焦点会话**。帧头的 `session`/`status`/`text` 永远是焦点会话——旧设备忽略 `bubbles` 即天然只演焦点，行为不变 |

**焦点规则**（daemon 侧 focus.js）：`waiting_confirm` 无条件抢占焦点（不错过任何会话的授权）；焦点会话超时淘汰后回落最新活跃；空闲态 `no` 键循环切换（见 §3.5）。

**状态语义（项目核心词汇，三方统一）**

| status | 语义 | 触发（Claude Code hook） |
|---|---|---|
| `thinking` | 思考中 | UserPromptSubmit / SessionStart |
| `working` | 执行工具中 | PreToolUse / PostToolUse |
| `waiting_confirm` | 等待用户授权 | Notification（含 permission 语义） |
| `done` | 回合完成 | Stop |
| `error` | 出错 | PostToolUseFailure 等（映射可扩展） |

### 3.5 button（device → daemon）

```json
{"ver":1,"type":"button","key":"yes","event":"short"}
```

| 字段 | 说明 |
|---|---|
| `key` | v1：`yes` / `no` / `btn3`（自定义键）；实体按键映射在固件 pinmap 定义 |
| `event` | `short` / `long` |

**语义**：`yes` + `short` = 授权当前 `waiting_confirm` 的 permission 请求（创新点 F03 的回注通道）。
`no`：有 permission 挂起 = 按下即拒；空闲（无挂起）= 循环切换多会话焦点（F02R）。

### 3.6 error（双向，可选）

```json
{"ver":1,"type":"error","code":"bad_frame","msg":"line >4KB"}
```

常用 `code`：`version_mismatch` / `bad_json` / `bad_frame` / `unknown_key`。发送 `error` 不意味着断连。

### 3.7 audio（device → daemon，语音输入上行）

一段录音拆成若干 `audio` 帧分块上行，`id` 相同的帧属于同一段，`end:true` 收尾：

```json
{"ver":1,"type":"audio","id":"a3","seq":0,"rate":16000,"end":false,"b64":"AAECwA..."}  // 数据块
{"ver":1,"type":"audio","id":"a3","seq":107,"end":true}                                 // 收尾
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | ✓ | 录音实例标识（设备侧自增，如 `"a3"`）；重组的主键 |
| `seq` | ✓ | 块序号，从 0 递增 |
| `end` | ✗ | `true` = 本段最后一块（收尾帧不带 `b64`） |
| `rate` | ✗ | 采样率 Hz（默认 16000，常出现在首块） |
| `b64` | ✗ | PCM 块的 base64：16bit 小端单声道裸数据 |

- 单帧上限同 §4（4KB）；发送方按 ~2.4KB b64 负载分块（固件 `PL_AUDIO_CHUNK=1800` 字节 PCM）。
- 重组方（daemon）按 `id` 拼接 `b64`，收到 `end` 后整体 base64 解码为 PCM 送 ASR。
- **可靠性语义（v1 简化）**：分块不经确认、不重传，乱序/丢失/超时（30s 未收尾）即弃段——
  语音输入是尽力而为的增强通道，卡死的半段绝不能占内存。设备录音时长典型 6s（192KB）。
- 触发语义由设备侧定义（实体设备：空闲态长按 YES；`waiting_confirm` 期间长按仍是拒绝手势）。

## 4. 传输承载

| 承载 | 帧化 | 用途 |
|---|---|---|
| USB CDC（实体设备） | NDJSON，`\n` 结尾 | MVP 主链路；v0.4 起波特率 921600（语音上行 32KB/s PCM 的带宽要求） |
| TCP | NDJSON | 调试回环、未来扩展 |
| WebSocket | 一帧一消息 | 浏览器虚拟设备 |

单帧上限 **4KB**；语音音频走 §3.7 的 `audio` 分块（应用层分块，不属帧层机制）；更大负载（资源包等）v1 不涉及，预留分块命名空间（`asset_*` 前缀），届时另行升版。

## 5. 版本演进规则

- 新增字段：不升版，旧端忽略（§1 前向兼容）。
- 新增消息类型：不升版，旧端忽略未知 `type`。
- 修改既有字段语义 / 删除字段：升 `ver`，hello 协商失败按 §1 处理。
