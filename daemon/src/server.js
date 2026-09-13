'use strict';
/**
 * companion-daemon 入口。
 * 用法：
 *   npm start -- --fake          # Day-1：假状态流（无 Agent）
 *   npm start                    # 正式：Claude Code hooks + Codex CLI watch
 * 端口：HTTP 7331（状态页+hooks） / WS 7332（虚拟设备） / TCP 7333（NDJSON 回环）
 *
 * 管道：Claude Code hooks（Post /state）+ Codex CLI JSONL watch（codex-watch.js）
 * ASR：优先 sherpa-onnx（asr-sherpa.js），回退 whisper
 */

const { LinkManager } = require('./links');
const { createHttpServer, PermissionManager } = require('./hooks');
const { SessionRegistry } = require('./state');
const { FocusManager } = require('./focus');
const { TokenTracker } = require('./tokens');
const { Stats } = require('./stats');
const { Voice } = require('./voice');
const { startFake } = require('./fake');
const inject = require('./inject');

// ---- Codex CLI JSONL watcher ----
let CodexWatch = null;
try { ({ CodexWatch } = require('./codex-watch')); } catch(e) {}

// ---- ASR 后端：优先 sherpa-onnx，回退 whisper ----
let transcribe = null;
let asrBackend = 'none';
try {
  ({ transcribe } = require('./asr-sherpa'));
  asrBackend = 'sherpa';
} catch(e) {}
if (!transcribe) {
  try { ({ transcribe } = require('./whisper')); asrBackend = 'whisper'; } catch(e) {}
}

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
const focus = new FocusManager(registry, log, (fr) => {
  const t = tokens.get(fr.session);
  if (t) fr.tokens = t;
  return fr;
});

async function pickPane(logfn) {
  if (process.env.PETLINK_INJECT_PANE) return process.env.PETLINK_INJECT_PANE;
  try { const panes = await inject.listPanes(); return panes.length === 1 ? panes[0].pane : null; } catch(e) { return null; }
}

async function handleAudio(dev, { id, rate, pcm }) {
  if (!transcribe) { log('[voice-in] ' + dev + ' ASR unavailable'); stats.voiceFail(); return; }
  stats.voiceStart();
  const text = await transcribe({ pcm, rate, log });
  if (!text) { stats.voiceFail(); return; }
  try {
    const pane = await pickPane(log);
    if (!pane) { stats.voiceFail(); return; }
    await inject.sendText({ pane, text, enter: true });
    stats.voiceOk(text);
    log('[voice-in] ' + dev + ' -> pane=' + pane + ': "' + text.slice(0, 60) + '"');
  } catch(e) { stats.voiceFail(); }
}

const links = new LinkManager({
  log,
  onButton: (key, event, dev) => {
    stats.button(key, event);
    if (perms.resolveButton(key, event, dev)) return;
    if (key === 'no') { const f = focus.cycle(); if (f) links.broadcast(f); }
  },
  onAudio: (dev, a) => { handleAudio(dev, a); },
});

// ---- Codex CLI watcher (常驻) ----
let cxWatch = null;
if (CodexWatch) {
  cxWatch = new CodexWatch({ registry, focus, stats, voice, links, log });
  log('[codex-watch] loaded');
}

const httpSrv = createHttpServer({ registry, links, perms, inject, stats, voice, focus, tokens, log, startedAt });
httpSrv.on('error', (e) => {
  if (e.code === 'EADDRINUSE') { log('port ' + OPTS.http + ' in use'); }
  else { log('HTTP listen failed: ' + e.message); }
  process.exit(1);
});
httpSrv.listen(OPTS.http, () => log('HTTP :' + OPTS.http + ' (status /hooks /device)'));

links.startWs(OPTS.ws);
links.startTcp(OPTS.tcp);
links.startUsb();

if (cxWatch) cxWatch.start();
if (OPTS.fake) startFake(registry, links, stats, voice, log);

log('pipes: Claude Code hooks + ' + (cxWatch ? 'Codex CLI watch' : 'no Codex') + ' | ASR: ' + asrBackend + ' | ' + process.platform);

const POLL = Number(process.env.PETLINK_TOKENS_POLL_MS) || 5000;
setInterval(() => {
  const s = focus.focus; if (!s) return;
  const b = tokens.get(s); const a = tokens.poll(s);
  if (!a || (b && b.ctx === a.ctx && b.tot === a.tot)) return;
  const f = focus.reframe(); if (f) links.broadcast(f);
}, POLL).unref();

for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { process.exit(0); });
