'use strict';
/**
 * HTTP 服务：
 *  - GET  /            localhost 状态页（连接/版本/测试 三件事）
 *  - GET  /status      同上，JSON 版
 *  - GET  /stats       会话统计（事件/状态/时长/permission/按键/语音计数器）
 *  - GET  /device/*    虚拟设备静态页（从 ../virtual_device 提供）
 *  - POST /state       Claude Code hook 事件入口（v1 手动/脚本 POST，W1 接 hooks 配置）
 *  - POST /permission  阻塞式授权：挂起等待设备 Yes 键，超时 deny
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const proto = require('./protocol');
const { normalizeHook } = require('./state');

const VIRTUAL_DEVICE_ROOT = path.resolve(__dirname, '..', '..', 'virtual_device');
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.webp': 'image/webp',
  '.svg': 'image/svg+xml',
};

/** 阻塞式 permission 仲裁：waiting_confirm 的回注通道（创新点 F03 的 daemon 侧） */
class PermissionManager {
  constructor(log) {
    this.log = log || (() => {});
    this.waiters = []; // FIFO
  }

  /**
   * 挂起一个授权请求，等设备按键或超时。
   * @returns {Promise<{outcome:'allow'|'deny'|'none', reason:string, by:string}>}
   *   none = 超时无人按键 → HTTP 层回空 body（Claude Code 语义：无决策，回落正常权限弹窗）
   */
  request(info, timeoutMs = 15000) {
    return new Promise((resolve) => {
      const w = {
        info: info || {},
        resolve: (outcome, reason, by) => {
          clearTimeout(w.timer);
          resolve({ outcome, reason, by });
        },
      };
      w.timer = setTimeout(() => {
        this.waiters.splice(this.waiters.indexOf(w), 1);
        w.resolve('none', 'timeout: 设备未在时限内按键', 'timeout');
      }, timeoutMs);
      this.waiters.push(w);
      this.log(`permission 挂起: ${JSON.stringify(w.info).slice(0, 120)}（等待 Yes 键，${timeoutMs}ms 超时→无决策）`);
    });
  }

  /** 设备按键路由：yes短按=allow，yes长按/no键=deny，其余忽略。@return 是否消费了按键 */
  resolveButton(key, event, dev) {
    const w = this.waiters[0];
    if (!w) return false;
    if (key === 'no') {
      // 独立 NO 键（v0.1.1 增补）：按下即拒，不看时长——600ms 长按在演示现场太难按准
      this.waiters.shift();
      w.resolve('deny', '用户按下实体 NO 键', `device:${dev}`);
      return true;
    }
    if (key === 'yes' && event === 'short') {
      this.waiters.shift();
      w.resolve('allow', '用户按下实体 Yes 键', `device:${dev}`);
      return true;
    }
    if (key === 'yes' && event === 'long') {
      this.waiters.shift();
      w.resolve('deny', '用户长按 Yes 键拒绝', `device:${dev}`);
      return true;
    }
    return false; // btn2/btn3 留给未来自定义动作
  }
}

function readBody(req) {
  return new Promise((resolve) => {
    let buf = '';
    req.on('data', (c) => {
      buf += c;
      if (buf.length > 1 << 20) req.destroy();
    });
    req.on('end', () => {
      try { resolve(JSON.parse(buf || '{}')); } catch { resolve(null); }
    });
  });
}

function createHttpServer({ registry, links, perms, inject, stats, voice, log, startedAt }) {
  const srv = http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    const json = (code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' }); res.end(JSON.stringify(obj)); };

    if (req.method === 'GET' && (url.pathname === '/' || url.pathname === '/status')) {
      return json(200, {
        app: 'companion-daemon',
        ver: require('../package.json').version,
        proto: `petlink/${proto.VER}`,
        uptime_s: ((Date.now() - startedAt) / 1000) | 0,
        devices: [...links.devices.entries()].map(([id, c]) => ({ id, label: c.label, ...c.meta })),
        sessions: registry.snapshot(),
      });
    }

    if (req.method === 'GET' && url.pathname.startsWith('/device')) {
      const rel = url.pathname === '/device' ? 'index.html' : url.pathname.slice('/device/'.length);
      const file = path.normalize(path.join(VIRTUAL_DEVICE_ROOT, rel));
      if (!file.startsWith(VIRTUAL_DEVICE_ROOT)) return json(403, { ok: false });
      fs.readFile(file, (err, data) => {
        if (err) return json(404, { ok: false, err: 'not_found' });
        res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
        res.end(data);
      });
      return;
    }

    if (req.method === 'GET' && url.pathname === '/stats') {
      return json(200, stats.snapshot());
    }

    if (req.method === 'POST' && url.pathname === '/state') {
      const body = await readBody(req);
      if (!body) return json(400, { ok: false, err: 'bad_json' });
      const eventName = body.event || body.hook_event_name || '';
      stats.event(eventName);
      const norm = normalizeHook(body);
      if (!norm) return json(422, { ok: false, err: 'unmapped_event', event: eventName || null });
      const session = String(body.session_id || body.session || 'default').slice(0, 64);
      const agent = String(body.agent || 'claude-code');
      registry.update(session, agent, norm.status, norm.text);
      stats.status(session, norm.status);
      voice.announceStatus(norm.status, norm.text);
      const n = links.broadcast({ type: 'state', session, agent, status: norm.status, text: norm.text });
      return json(202, { ok: true, broadcast: n, session, status: norm.status });
    }

    if (req.method === 'POST' && url.pathname === '/permission') {
      const body = await readBody(req);
      if (!body) return json(400, { ok: false, err: 'bad_json' });
      const timeout = Math.min(Math.max(+body.timeout_ms || 15000, 1000), 60000);
      const out = await perms.request({ session_id: body.session_id, tool: body.tool, prompt: body.prompt }, timeout);
      stats.permissionOutcome(out.outcome);
      voice.announcePermission(out.outcome);

      // Claude Code PermissionRequest HTTP hook 语义（官方 hooks 文档）：
      //  - 2xx 空 body = 无决策 → 回落正常权限弹窗（超时/设备不在时的自然兜底）
      //  - decision.behavior = allow / deny；deny 可带 message 告知原因
      if (out.outcome === 'none') {
        log(`permission 结果: 无决策（${out.reason}）→ 回落终端弹窗`);
        // 授权等待已结束：把设备从 waiting_confirm 拉回 thinking。超时回落后
        // agent 可能长时间停在终端弹窗等人应答，不清就永远显示"等待授权"
        // （2026-09-08 真机教训：卡 13 分钟，期间设备长按 YES 全被拒绝语义
        // 吞掉、语音输入也被 waiting 门控拦死）
        const sess = String(body.session_id || body.session || 'default').slice(0, 64);
        const agent = String(body.agent || 'claude-code');
        registry.update(sess, agent, 'thinking', '');
        stats.status(sess, 'thinking');
        links.broadcast({ type: 'state', session: sess, agent, status: 'thinking', text: '' });
        res.writeHead(200);
        return res.end();
      }
      log(`permission 结果: ${out.outcome}（by=${out.by}）`);
      const payload = {
        hookSpecificOutput: {
          hookEventName: 'PermissionRequest',
          decision: { behavior: out.outcome },
        },
      };
      if (out.outcome === 'deny') payload.hookSpecificOutput.decision.message = out.reason;
      return json(200, payload);
    }

    // ---- 烧录窗口：让 daemon 松开 USB 串口，esptool 才能打开（否则 Resource busy） ----
    if (req.method === 'POST' && url.pathname === '/usb/pause') {
      const ok = links.pauseUsb();
      return json(ok ? 200 : 404, { ok, msg: ok ? 'USB 已释放，烧录完 POST /usb/resume 恢复' : 'USB 链路未启用' });
    }
    if (req.method === 'POST' && url.pathname === '/usb/resume') {
      const ok = links.resumeUsb();
      return json(ok ? 200 : 404, { ok, msg: ok ? 'USB 恢复扫描' : 'USB 链路未启用' });
    }

    // ---- tmux 注入（宪法 §7 注入 adapter 的文本路径） ----
    if (req.method === 'GET' && url.pathname === '/inject/panes') {
      try {
        const panes = await inject.listPanes();
        return json(200, { ok: true, panes });
      } catch (e) {
        return json(422, { ok: false, err: e.code === 'ENOENT' ? 'tmux_missing' : 'tmux_error', hint: e.code === 'ENOENT' ? 'brew install tmux 并启动会话' : String(e.stderr || e.message).slice(0, 160) });
      }
    }
    if (req.method === 'POST' && url.pathname === '/inject') {
      const body = await readBody(req);
      if (!body || !body.pane || !body.text) return json(400, { ok: false, err: 'need pane,text' });
      try {
        await inject.sendText(body);
        log(`[inject] pane=${body.pane} text="${String(body.text).slice(0, 40)}"`);
        return json(200, { ok: true, pane: body.pane });
      } catch (e) {
        return json(422, { ok: false, err: e.code === 'ENOENT' ? 'tmux_missing' : 'tmux_error', hint: e.code === 'ENOENT' ? 'brew install tmux 并启动会话' : String(e.stderr || e.message).slice(0, 160) });
      }
    }

    json(404, { ok: false });
  });
  return srv;
}

module.exports = { createHttpServer, PermissionManager };
