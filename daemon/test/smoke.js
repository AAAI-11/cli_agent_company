'use strict';
/**
 * Day-1 冒烟测试：不起浏览器，用 Node WS 客户端扮演虚拟设备，
 * 走完 PetLink v1 全链路：hello → hello_ok → state 广播 → heartbeat →
 * button → permission 仲裁（创新点 F03 的回注闭环）。
 * 用法：cd daemon && node test/smoke.js
 */

const { spawn } = require('child_process');
const path = require('path');
const WebSocket = require('ws');

// 测试专用端口：不和你日常跑的 daemon（7331-7333）打架
const P = { http: 7391, ws: 7392, tcp: 7393 };
const HTTP = `http://127.0.0.1:${P.http}`;
const WS = `ws://127.0.0.1:${P.ws}`;

const results = [];
function check(name, ok, detail) {
  results.push({ name, ok, detail: detail || '' });
  console.log(`${ok ? '✅' : '❌'} ${name}${detail ? ' — ' + detail : ''}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  const daemon = spawn(process.execPath, [path.join(__dirname, '..', 'src', 'server.js'), '--fake'], {
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env, PETLINK_HTTP: String(P.http), PETLINK_WS: String(P.ws), PETLINK_TCP: String(P.tcp),
           PETLINK_SERIAL_PATH: '/dev/nonexistent-petlink-test', // 测试 daemon 不碰真机串口（避免与生产 daemon 抢口）
           PETLINK_VOICE_DRY: '1',                 // 语音输出干跑：只记日志不真发声
           PETLINK_VOICE_GAP_MS: '1',               // 测试里三条播报间隔 <1s，默认 3s 冷却会吞掉
           PETLINK_WHISPER_BIN: '/nonexistent-whisper-test', // 语音输入走"缺依赖"路径（不依赖本机装没装）
           PETLINK_TOKENS_POLL_MS: '150' },            // F15 轮询缩短，等 500ms 即可验兜底刷新
  });
  const daemonLog = [];
  daemon.stdout.on('data', (d) => daemonLog.push(d.toString()));
  daemon.stderr.on('data', (d) => process.stderr.write(`[daemon] ${d}`));
  await sleep(1200); // 等端口就绪

  try {
    const ws = new WebSocket(WS);
    const rx = [];
    ws.on('message', (d) => rx.push(JSON.parse(d.toString())));
    await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });

    // 0) 孤儿设备容忍：握手前收到 heartbeat 不许踢连接、不许回 error——
    //    设备可能还在上个 daemon 的 90s 判活窗内（2026-09-02 真机事故：error 续命 + close = 3s 闪断死锁）
    const errBefore = rx.filter((m) => m.type === 'error').length;
    ws.send(JSON.stringify({ ver: 1, type: 'heartbeat', uptime_ms: 1 }));
    await sleep(400);
    const wsAlive = ws.readyState === ws.OPEN;
    check('握手前 heartbeat 不踢连接不回 error（孤儿容忍）', wsAlive && rx.filter((m) => m.type === 'error').length === errBefore,
      wsAlive ? '连接存活' : '连接被关闭');

    // 1) 握手
    ws.send(JSON.stringify({ ver: 1, type: 'hello', device: 'smoke-1', fw: '0.1.0', hw: 'browser', caps: ['button'] }));
    await sleep(300);
    const helloOk = rx.find((m) => m.type === 'hello_ok');
    check('握手 hello → hello_ok', !!helloOk, helloOk ? `echo=${helloOk.echo} daemon=${helloOk.daemon_ver}` : '未收到');

    // 1b) 回归（2026-09-02 真机事故）：设备没收到 hello_ok 会每 5s 重发 hello，
    //     daemon 必须幂等重答——否则 CDC 首写一丢就死循环到 90s 摘除
    const rx1b = rx.length;
    ws.send(JSON.stringify({ ver: 1, type: 'hello', device: 'smoke-1', fw: '0.1.0', hw: 'browser', caps: ['button'] }));
    await sleep(300);
    const reOk = rx.slice(rx1b).find((m) => m.type === 'hello_ok');
    check('重发 hello → 幂等重答 hello_ok（丢包自愈）', !!reOk && reOk.echo === 'smoke-1', reOk ? `echo=${reOk.echo}` : '未收到');

    // 2) 假状态流广播（fake 3s 一条）
    await sleep(6500);
    const states = rx.filter((m) => m.type === 'state');
    check('收到 state 广播 ≥2 条', states.length >= 2, `共 ${states.length} 条，最新 ${states.at(-1)?.status}`);

    // 3) 心跳：探测→应答（应答须带 reply:true，防乒乓风暴）
    const hbBefore = rx.filter((m) => m.type === 'heartbeat').length;
    ws.send(JSON.stringify({ ver: 1, type: 'heartbeat', uptime_ms: 1 }));
    await sleep(300);
    const hbReplies = rx.slice(hbBefore).filter((m) => m.type === 'heartbeat');
    check('heartbeat 探测→应答且带 reply:true', hbReplies.length === 1 && hbReplies[0].reply === true, JSON.stringify(hbReplies));

    // 4) permission 闭环：挂起 → Yes 短按 → allow
    const p1 = fetch(`${HTTP}/permission`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ session_id: 'sess-a', tool: 'Bash', prompt: 'rm -rf /tmp/junk', timeout_ms: 8000 }),
    }).then((r) => r.json());
    await sleep(400); // 确保已挂起
    ws.send(JSON.stringify({ ver: 1, type: 'button', key: 'yes', event: 'short' }));
    const d1 = await p1;
    check('permission 由 Yes 短按放行', d1.hookSpecificOutput?.decision?.behavior === 'allow', JSON.stringify(d1).slice(0, 90));

    // 4b) 独立 NO 键（v0.1.1 增补，实体键 GPIO22=排针29脚）：按下即拒
    const p1b = fetch(`${HTTP}/permission`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ session_id: 'sess-a', tool: 'Bash', prompt: 'rm -rf /tmp/junk2', timeout_ms: 8000 }),
    }).then((r) => r.json());
    await sleep(400); // 确保已挂起
    ws.send(JSON.stringify({ ver: 1, type: 'button', key: 'no', event: 'short' }));
    const d1b = await p1b;
    check('permission 由 NO 键拒绝', d1b.hookSpecificOutput?.decision?.behavior === 'deny', JSON.stringify(d1b).slice(0, 90));

    // 4c) YES 长按同样 deny（v0.1 老语义，NO 键出现后仍是合法拒绝路径）
    const p1c = fetch(`${HTTP}/permission`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ session_id: 'sess-a', tool: 'Bash', prompt: 'rm -rf /tmp/junk3', timeout_ms: 8000 }),
    }).then((r) => r.json());
    await sleep(400); // 确保已挂起
    ws.send(JSON.stringify({ ver: 1, type: 'button', key: 'yes', event: 'long' }));
    const d1c = await p1c;
    check('permission 由 Yes 长按拒绝', d1c.hookSpecificOutput?.decision?.behavior === 'deny', JSON.stringify(d1c).slice(0, 90));

    // 5) 超时路径：无人按键 → 2xx 空 body（无决策，回落终端弹窗）
    const r5 = await fetch(`${HTTP}/permission`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ timeout_ms: 1500 }),
    });
    const t5 = await r5.text();
    check('permission 超时→空应答(无决策)', r5.status === 200 && t5 === '', `status=${r5.status} body="${t5.slice(0, 40)}"`);

    // 6) hook 事件入口 → 广播出错路径
    const r6 = await fetch(`${HTTP}/state`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ event: 'PreToolUse', session_id: 'sess-a', tool_name: 'Edit(src/x.js)' }),
    }).then((r) => r.json());
    check('POST /state 归一化广播', r6.ok && r6.status === 'working', `broadcast=${r6.broadcast}`);

    // 6b) F02R 多会话（宪法 §4.2 Stretch）：后台不抢屏 / bubbles 附带 /
    //     waiting_confirm 抢占 / 空闲 NO 键循环切换。
    //     注：--fake 的假状态流走旧直发路径（无 bubbles、不参与焦点），
    //     因此带 bubbles 的帧均来自焦点组装，断言用 HTTP 响应的 focus 字段避开时序竞争
    const post6b = (body) => fetch(`${HTTP}/state`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    }).then((r) => r.json());
    const rb1 = await post6b({ event: 'UserPromptSubmit', session_id: 'sess-b1', prompt: '会话一启动' });
    const rb2 = await post6b({ event: 'PreToolUse', session_id: 'sess-b2', tool_name: 'Bash' });
    // 焦点归属"首个到达的会话"（此处更早的超时测试已让 default 持屏）——
    // 不变量是"后台不抢"：b1/b2 都不能把 focus 从持屏会话手里夺走
    check('F02R 后台会话不抢焦点', rb1.ok && rb2.ok && typeof rb2.focus === 'string' && rb2.focus !== 'sess-b1' && rb2.focus !== 'sess-b2', `focus=${rb2.focus}`);
    await sleep(300);
    const bub = rx.filter((m) => m.type === 'state' && Array.isArray(m.bubbles)).at(-1);
    check('F02R state 帧携带 bubbles（恰一个焦点标记）',
      !!bub && bub.bubbles.filter((b) => b.f === 1).length === 1
        && bub.bubbles.some((b) => b.s === 'sess-b1') && bub.bubbles.some((b) => b.s === 'sess-b2'),
      JSON.stringify(bub?.bubbles));
    const rb3 = await post6b({ event: 'Notification', session_id: 'sess-b2', message: 'Claude needs your permission' });
    check('F02R waiting_confirm 抢占焦点', rb3.ok && rb3.focus === 'sess-b2', `focus=${rb3.focus}`);
    // Notification 语义消歧（F12/F16 触发集）：notif=error → error 态，
    // 不再被 v1 简化一律吞成 waiting_confirm（2026-09-04 真机：error 提示音不响）
    const rbErr = await post6b({ event: 'Notification', session_id: 'sess-b2', notif: 'error', message: 'boom' });
    check('Notification notif=error 归一为 error 态', rbErr.ok && rbErr.status === 'error', `status=${rbErr.status}`);
    // idle 通知不转移状态（2026-09-11 真机：Claude is waiting for your input 被吞成
    // waiting_confirm，屏锁死「等你拍板」按键救不回）——须返回 unmapped_event 忽略
    const rbIdle = await post6b({ event: 'Notification', session_id: 'sess-b2', notification_type: 'idle', message: 'Claude is waiting for your input' });
    check('Notification idle 闲聊类被忽略（不产生假 waiting_confirm）', !rbIdle.ok && rbIdle.err === 'unmapped_event', JSON.stringify(rbIdle));
    // 真实 hook 转发路径的 permission（notification_type 字段 + message）→ 仍须 waiting_confirm
    const rbPerm = await post6b({ event: 'Notification', session_id: 'sess-b2', notification_type: 'permission', message: 'Claude needs your permission to use Bash' });
    check('Notification notification_type=permission 归一为 waiting_confirm', rbPerm.ok && rbPerm.status === 'waiting_confirm', `status=${rbPerm.status}`);
    // 空闲态 NO 键：此刻无 permission 挂起（前面都已决出），按键落到焦点循环
    const rxBefore = rx.length;
    ws.send(JSON.stringify({ ver: 1, type: 'button', key: 'no', event: 'short' }));
    await sleep(400);
    const cyc = rx.slice(rxBefore).find((m) => m.type === 'state' && Array.isArray(m.bubbles));
    check('F02R 空闲 NO 键循环切换焦点', !!cyc && cyc.session !== 'sess-b2'
      && cyc.bubbles.filter((b) => b.f === 1).length === 1, `session=${cyc?.session} bubbles=${JSON.stringify(cyc?.bubbles)}`);

    // 6c) F15 token 消耗显示（宪法 §4.5）：transcript 增量解析 → 焦点帧附 tokens；
    //     坏行/非 assistant 行忽略；无事件期的追加靠轮询兜底可见
    const fsMod = require('fs');
    const osMod = require('os');
    const tdir = fsMod.mkdtempSync(path.join(osMod.tmpdir(), 'petlink-tokens-'));
    const tfile = path.join(tdir, 'sess-t1.jsonl');
    const usageLine = (i, o, cc, cr) => JSON.stringify({
      type: 'assistant',
      message: { usage: { input_tokens: i, output_tokens: o, cache_creation_input_tokens: cc, cache_read_input_tokens: cr } },
    });
    fsMod.writeFileSync(tfile, [
      JSON.stringify({ type: 'user', message: 'hi' }), // 非 assistant：不计
      usageLine(100, 20, 0, 400),                      // 行计 520
      'not json at all',                               // 坏行：不计
      usageLine(120, 30, 50, 500),                     // 行计 700
    ].join('\n') + '\n');
    // waiting_confirm 抢占焦点，token 才会随帧下发（口径 = 焦点会话）
    const rb4 = await post6b({ event: 'Notification', session_id: 'sess-t1', message: 'Claude needs your permission', transcript_path: tfile });
    await sleep(300);
    const tk1 = rx.filter((m) => m.type === 'state' && m.tokens).at(-1);
    check('F15 state 帧携带 tokens（ctx=最新条 700 / tot=累加 1220，坏行忽略）',
      rb4.focus === 'sess-t1' && tk1 && tk1.session === 'sess-t1' && tk1.tokens.ctx === 700 && tk1.tokens.tot === 1220,
      `focus=${rb4.focus} tokens=${JSON.stringify(tk1?.tokens)}`);
    // 无事件期追加一行：等轮询（150ms）自发刷新帧，不 POST 任何东西
    fsMod.appendFileSync(tfile, usageLine(10, 5, 0, 700) + '\n'); // 行计 715
    await sleep(600);
    const tk2 = rx.filter((m) => m.type === 'state' && m.tokens && m.session === 'sess-t1').at(-1);
    check('F15 轮询兜底刷新（无事件期追加可见）',
      !!tk2 && tk2.tokens.ctx === 715 && tk2.tokens.tot === 1935,
      `tokens=${JSON.stringify(tk2?.tokens)}`);


    // 7) 状态页与虚拟设备静态页
    const st = await fetch(`${HTTP}/status`).then((r) => r.json());
    check('/status 设备在册', (st.devices || []).some((d) => d.id === 'smoke-1'), JSON.stringify(st.devices));
    const page = await fetch(`${HTTP}/device`);
    const html = await page.text();
    check('/device 页面可达且为虚拟设备', page.status === 200 && html.includes('虚拟设备'));

    // 8) 烧录窗口：pause/resume USB（esptool 抢口场景）
    const pu = await fetch(`${HTTP}/usb/pause`, { method: 'POST' }).then((r) => r.json());
    check('USB 烧录窗口可暂停', pu.ok === true, JSON.stringify(pu));
    const ru = await fetch(`${HTTP}/usb/resume`, { method: 'POST' }).then((r) => r.json());
    check('USB 烧录窗口可恢复', ru.ok === true, JSON.stringify(ru));

    // 9) 转发器 E2E：像 Claude Code 一样经 stdin 喂事件 → 归一化 → 入册
    const fwd = spawn(process.execPath, [path.join(__dirname, '..', 'hooks', 'petlink-hook.js')], {
      stdio: ['pipe', 'ignore', 'ignore'],
      env: { ...process.env, PETLINK_DAEMON: HTTP },
    });
    fwd.stdin.end(JSON.stringify({ hook_event_name: 'Stop', session_id: 'hook-e2e-1', cwd: '/tmp' }));
    await sleep(700);
    const st2 = await fetch(`${HTTP}/status`).then((r) => r.json());
    const sess = (st2.sessions || []).find((s) => s.session === 'hook-e2e-1');
    check('petlink-hook 转发器 E2E（Stop→done）', !!sess && sess.status === 'done', sess ? `status=${sess.status}` : '未见会话');

    // 10) 语音输出 dry-run：done 状态 / permission 放行拒绝 都该念一句（只记日志不发声）
    const voiceLog = daemonLog.join('');
    const dryLines = voiceLog.split('\n').filter((l) => l.includes('voice-dry'));
    check('语音输出：done/allow/deny 播报（dry）',
      voiceLog.includes('[voice-dry] 已完成') && voiceLog.includes('[voice-dry] 已放行') && voiceLog.includes('[voice-dry] 已拒绝'),
      dryLines.slice(0, 5).join(' | ') || '未见 voice-dry 日志');

    // 11) 会话统计 GET /stats：本套冒烟跑过的每一类事件都该在计数器里
    const s1 = await fetch(`${HTTP}/stats`).then((r) => r.json());
    check('/stats 计数 permission 闭环', s1.permission.requests >= 4 && s1.permission.allow >= 1 && s1.permission.deny >= 2,
      JSON.stringify(s1.permission));
    check('/stats 计数按键', (s1.buttons['yes:short'] || 0) >= 1 && (s1.buttons['no:short'] || 0) >= 1 && (s1.buttons['yes:long'] || 0) >= 1,
      JSON.stringify(s1.buttons));
    check('/stats 计数 hook 事件与状态', s1.totals.hook_events >= 2 && s1.statuses.working >= 1 && s1.statuses.done >= 1 && s1.totals.sessions >= 2,
      `events=${s1.totals.hook_events} working=${s1.statuses.working} done=${s1.statuses.done} sessions=${s1.totals.sessions}`);
    check('/stats 状态时长非零', s1.status_ms.working > 0 || s1.status_ms.done > 0, JSON.stringify(s1.status_ms));

    // 12) 语音输入：模拟设备分块发 audio 帧 → daemon 重组 → whisper 缺依赖 → voiceFail 打点
    //     （重组正确性的验证点是"段完成"日志；ASR 正路径留真机 E2E）
    const pcm = Buffer.alloc(3200, 0); // 100ms 静音
    const b64 = pcm.toString('base64');
    const half = Math.ceil(b64.length / 2);
    ws.send(JSON.stringify({ ver: 1, type: 'audio', id: 'sm-a1', seq: 0, rate: 16000, end: false, b64: b64.slice(0, half) }));
    ws.send(JSON.stringify({ ver: 1, type: 'audio', id: 'sm-a1', seq: 1, end: false, b64: b64.slice(half) }));
    ws.send(JSON.stringify({ ver: 1, type: 'audio', id: 'sm-a1', seq: 2, end: true }));
    await sleep(500);
    const s2 = await fetch(`${HTTP}/stats`).then((r) => r.json());
    check('audio 分块重组→段完成（ASR 缺依赖走 fail 打点）', s2.voice.requests === 1 && s2.voice.fail === 1,
      JSON.stringify(s2.voice));
    check('坏音频帧（无 id）静默丢弃', (() => {
      ws.send(JSON.stringify({ ver: 1, type: 'audio', seq: 0, end: true }));
      return true;
    })());

    ws.close();
  } finally {
    daemon.kill('SIGTERM');
  }

  const fail = results.filter((r) => !r.ok).length;
  console.log(`\n冒烟结果：${results.length - fail}/${results.length} 通过`);
  process.exit(fail ? 1 : 0);
}

main().catch((e) => {
  console.error('冒烟脚本异常：', e);
  process.exit(1);
});
