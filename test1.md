# v0.6.0 真机验收单（F02R 多会话气泡 + F15 token 消耗显示)

> 环境：daemon 已在跑（终端 A，`~/project_lh/cli_agent_company/daemon` + `npm start`），
> 板子已烧 **fw=0.6.0** 并握手（`/status` 里 `p4-e396bd` 在册）。
> 以下命令全部在**终端 B**执行；✅ = 预期现象。打勾记录：[ ]

## 一、F02R 多会话气泡（手册 §14）

### a. [ ] 造两个后台会话（两条先后发）

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"UserPromptSubmit","session_id":"sess-a","prompt":"会话A启动"}'
```

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"PreToolUse","session_id":"sess-b","tool_name":"Bash"}'
```

✅ 屏上演 **sess-a**（先到先得），顶栏出现**两个气泡**，sess-a 那个加大带白环；
两条 curl 返回 JSON 的 `"focus":"sess-a"`（后台会话不抢屏）。

### b. [ ] waiting_confirm 无条件抢占焦点

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"Notification","session_id":"sess-b","message":"Claude needs your permission"}'
```

✅ 屏立刻切到 sess-b「等你拍板」，白环跳到 sess-b 气泡——平时不抢屏，
**授权请求无条件抢**。curl 返回 `"focus":"sess-b"`。

### c. [ ] 空闲态 NO 键循环切换焦点

直接**按板上 NO 键**（此刻无授权挂起）。

✅ 屏切回 sess-a，白环跟走；daemon 终端 A 打出
`[focus] NO 键切换焦点 → sess-a`。浏览器虚拟设备（http://localhost:7331/device）
点 NO 键同效——一实一虚同源。

### d. [ ] 有授权挂起时 NO 仍是"按下即拒"（语义不被切换偷走）

```bash
curl -sX POST localhost:7331/permission -H 'Content-Type: application/json' -d '{"session_id":"sess-b","tool":"Bash","prompt":"echo hi","timeout_ms":20000}'
```

这条会挂住——**20 秒内按板上 NO 键**。

✅ curl 立刻返回 `"behavior":"deny"`（不是切换焦点）。

## 二、F15 token 消耗显示（手册 §15）

### e. [ ] 造假 transcript + 带路径发状态

```bash
printf '%s\n' '{"type":"user","message":"hi"}' '{"type":"assistant","message":{"usage":{"input_tokens":100,"output_tokens":20,"cache_creation_input_tokens":0,"cache_read_input_tokens":400}}}' > /tmp/petlink_tok.jsonl
```

```bash
curl -sX POST localhost:7331/state -H 'Content-Type: application/json' -d '{"event":"UserPromptSubmit","session_id":"sess-a","prompt":"验token","transcript_path":"/tmp/petlink_tok.jsonl"}'
```

✅ 标题 PETLINK/1 下方出现常驻小字 **`ctx 0.5k / tot 0.5k`**（100+20+0+400=520）。
前提：sess-a 持屏（b 步后若焦点在 sess-b，先按 NO 切回）。
虚拟设备同款底行同步显示。

### f. [ ] 增量追加（验证 5s 轮询兜底，不用再发 curl）

```bash
printf '%s\n' '{"type":"assistant","message":{"usage":{"input_tokens":120,"output_tokens":30,"cache_creation_input_tokens":50,"cache_read_input_tokens":500}}}' >> /tmp/petlink_tok.jsonl
```

等 5 秒 → token 行自动变 **`ctx 0.7k / tot 1.2k`**（tot=520+700=1220）。

### g. [ ] 真实数据（演示现场的样子）

随便开个 Claude Code 会话让它干活（手册 §6），底行自动出现并持续增长。
看一眼即可，不用截图。

## 三、收尾

- [ ] 全部通过 → 手册 F02R/F15 两行的 ⏳ 改 ✅，git 提交
- [ ] daemon 终端 A 保持跑着（演示还要用）；要关就 `Ctrl+C`

## 附：排查速查

| 症状 | 查法 |
|---|---|
| 屏上没气泡 | monitor 有无 `pet_ui: bubbles=N focus@i(...)`；N=0 则 `/status` 看会话在不在 |
| token 行没出现 | `/status` 的 `tokens` 字段有无 sess-a；monitor 里 `tokens=y` 吗 |
| 数字不动 | 长回合正常等 5s；`cat /tmp/petlink_tok.jsonl` 确认追加成功 |
| 按键没反应 | 手册 §9 排查三连（脚位/nc 发 button 帧/monitor 看日志） |
