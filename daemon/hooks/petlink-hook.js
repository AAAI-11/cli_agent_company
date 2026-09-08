#!/usr/bin/env node
'use strict';
/**
 * petlink-hook：Claude Code command hook 转发器。
 * Claude Code 每次触发 hook 事件 → 本脚本从 stdin 收事件 JSON →
 * 异步 POST 给 daemon /state → 立即退出 0（绝不阻塞 Agent）。
 *
 * 设计约束：
 *  - 任何失败（daemon 不在、JSON 坏、网络拒）都静默吞掉，退出码恒 0
 *  - 停留时间 < 300ms：fire-and-forget，不 await 响应体
 */

const DAEMON = process.env.PETLINK_DAEMON || 'http://127.0.0.1:7331';

let buf = '';
const stdin = process.stdin;
stdin.setEncoding('utf8');
stdin.on('data', (c) => { buf += c; });
stdin.on('end', () => {
  let ev = {};
  try { ev = JSON.parse(buf); } catch { /* 坏输入也静默 */ }

  const body = JSON.stringify({
    event: ev.hook_event_name || '',
    session_id: ev.session_id || '',
    transcript_path: ev.transcript_path || '',
    cwd: ev.cwd || '',
    tool_name: ev.tool_name || '',
    prompt: ev.prompt || '',
    message: ev.message || '',
    notification_type: ev.notification_type || '',
    agent: 'claude-code',
  });

  // fire-and-forget：发出即可，进程在事件循环空转后自然退出
  fetch(`${DAEMON}/state`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body,
    signal: AbortSignal.timeout(250),
  }).catch(() => {});

  // 双保险：250ms 后无论网络挂着什么都退场
  setTimeout(() => process.exit(0), 280).unref();
});
stdin.on('error', () => process.exit(0));

// stdin 意外没有数据到达时也别挂死
setTimeout(() => process.exit(0), 1500).unref();
