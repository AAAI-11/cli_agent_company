'use strict';
/**
 * companion-daemon 入口。
 * 用法：
 *   npm start -- --fake          # Day-1：假状态流（无 Claude Code）
 *   npm start                    # 正式：等待 hooks POST /state
 * 端口：HTTP 7331（状态页+hooks） / WS 7332（虚拟设备） / TCP 7333（NDJSON 回环）
 */

const { LinkManager } = require('./links');
const { createHttpServer, PermissionManager } = require('./hooks');
const { SessionRegistry } = require('./state');
const { FocusManager } = require('./focus');
const { TokenTracker } = require('./tokens');
const { Stats } = require('./stats');
const { Voice } = require('./voice');
const { transcribe } = require('./whisper');
const { startFake } = require('./fake');
const inject = require('./inject');

const args = process.argv.slice(2);
const OPTS = {
  fake: args.includes('--fake'),
  http: Number(process.env.PETLINK_HTTP) || 7331,
  ws: Number(process.env.PETLINK_WS) || 7332,
  tcp: Number(process.env.PETLINK_TCP) || 7333,
};

const startedAt = Date.now();
const ts = () => new Date().toISOString().slice(11, 23);
const log = (m) => console.log(`${ts()} ${m}`);

const registry = new SessionRegistry();
const perms = new PermissionManager(log);
const stats = new Stats();
const voice = new Voice(log);
const tokens = new TokenTracker(log);
// F15：帧出口统一装饰——焦点会话的 state 帧附带其 token 快照（协议 §3.4 可选字段）
const focus = new FocusManager(registry, log, (fr) => {
  const t = tokens.get(fr.session);
  if (t) fr.tokens = t;
  return fr;
});

/** 语音输入 → tmux 注入目标：PETLINK_INJECT_PANE 指定；未指定且唯一 pane 时选它；否则放弃 */
async function pickPane(logfn) {
  if (process.env.PETLINK_INJECT_PANE) return process.env.PETLINK_INJECT_PANE;
  const panes = await inject.listPanes();
  if (panes.length === 1) return panes[0].pane;
  if (panes.length === 0) return null;
  logfn(`[voice-in] 多个 tmux pane（${panes.map((p) => p.pane).join(', ')}），未设 PETLINK_INJECT_PANE，放弃注入`);
  return null;
}

/** 设备音频段完整到达（links.js 重组完）：ASR → 注入活动 pane */
async function handleAudio(dev, { id, rate, pcm }) {
  stats.voiceStart();
  const text = await transcribe({ pcm, rate, log });
  if (!text) {
    stats.voiceFail();
    log(`[voice-in] ${dev} 段 ${id} 无转写结果（whisper 缺依赖或空音频）`);
    return;
  }
  try {
    const pane = await pickPane(log);
    if (!pane) { stats.voiceFail(); return; }
    await inject.sendText({ pane, text, enter: true });
    stats.voiceOk(text);
    log(`[voice-in] ${dev} 语音已注入 pane=${pane}: "${text.slice(0, 60)}"`);
  } catch (e) {
    stats.voiceFail();
    log(`[voice-in] 注入失败：${e.message}`);
  }
}

const links = new LinkManager({
  log,
  onButton: (key, event, dev) => {
    stats.button(key, event);
    const consumed = perms.resolveButton(key, event, dev);
    if (consumed) {
      log(`[button] key=${key} event=${event} dev=${dev} → 已消费（permission 仲裁）`);
      return;
    }
    // F02R：空闲态 NO 键 = 循环切换焦点会话（有授权挂起时按键已被上面消费，
    // 走不到这里——"按下即拒"语义不被切换偷走）
    if (key === 'no') {
      const frame = focus.cycle();
      if (frame) {
        links.broadcast(frame);
        log(`[button] key=${key} event=${event} dev=${dev} → 切换焦点会话`);
        return;
      }
    }
    log(`[button] key=${key} event=${event} dev=${dev} → 无消费者（预留自定义动作）`);
  },
  onAudio: (dev, a) => { handleAudio(dev, a); },
});

const httpSrv = createHttpServer({ registry, links, perms, inject, stats, voice, focus, tokens, log, startedAt });
httpSrv.on('error', (e) => {
  if (e.code === 'EADDRINUSE') {
    log(`✗ 端口 ${OPTS.http} 已被占用——多半是已有一个 daemon 在跑。先停旧的再起：`);
    log(`  pkill -f "node src/server.js"`);
  } else {
    log(`✗ HTTP 监听失败: ${e.message}`);
  }
  process.exit(1);
});
httpSrv.listen(OPTS.http, () => log(`HTTP 监听 :${OPTS.http}（状态页 / 、hooks /state /permission、注入 /inject、虚拟设备 /device）`));
links.startWs(OPTS.ws);
links.startTcp(OPTS.tcp);
links.startUsb();

if (OPTS.fake) {
  startFake(registry, links, stats, voice, log);
  log('假状态流已启动（--fake）');
}

// F15：长时间回合里 transcript 一直长而 hook 事件未到——轮询焦点会话兜底刷新
// （transcript_path 只在 hook 事件里出现，所以轮询复用 tracker 里存的路径）
const TOKENS_POLL_MS = Number(process.env.PETLINK_TOKENS_POLL_MS) || 5000;
setInterval(() => {
  const s = focus.focus;
  if (!s) return;
  const before = tokens.get(s);
  const after = tokens.poll(s);
  if (!after || (before && before.ctx === after.ctx && before.tot === after.tot)) return;
  const frame = focus.reframe();
  if (frame) links.broadcast(frame);
}, TOKENS_POLL_MS).unref();

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    log('退出');
    process.exit(0);
  });
}
