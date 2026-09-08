'use strict';
/**
 * 假状态流：Day-1 无 Claude Code 时的状态源。
 * --fake 启用；接真实 hooks 后停用。
 */

const SEQ = [
  { status: 'thinking', text: '梳理需求边界' },
  { status: 'working', text: '修改 protocol.js' },
  { status: 'waiting_confirm', text: '请求执行 rm -rf' },
  { status: 'done', text: '回合完成，等待新指令' },
  { status: 'error', text: '测试命令超时' },
];

function startFake(registry, links, stats, voice, log, intervalMs = 3000) {
  let i = -1;
  const tick = () => {
    i = (i + 1) % SEQ.length;
    const s = SEQ[i];
    registry.update('fake-1', 'claude-code', s.status, s.text);
    if (stats) stats.status('fake-1', s.status);
    if (voice) voice.announceStatus(s.status, s.text);
    links.broadcast({ type: 'state', session: 'fake-1', agent: 'claude-code', status: s.status, text: s.text });
    log(`[fake] state → ${s.status}（${s.text}）`);
  };
  tick();
  const t = setInterval(tick, intervalMs);
  t.unref();
  return t;
}

module.exports = { startFake };
