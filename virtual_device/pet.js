'use strict';
/**
 * 虚拟设备：PetLink v1 over WebSocket（帧语义 = NDJSON 一行，见 docs/protocol.md §4）
 * 240×320 逻辑分辨率（与实体 2.8" 屏一致的竖屏心智），CSS 放大显示。
 */

/* ---------- 状态 ---------- */

const STATE_STYLE = {
  thinking:        { color: '#7aa2f7', zh: '思考中' },
  working:         { color: '#9ece6a', zh: '干活中' },
  waiting_confirm: { color: '#e0af68', zh: '等你拍板' },
  done:            { color: '#73daca', zh: '完成' },
  error:           { color: '#f7768e', zh: '出错了' },
};

const AGENT_STYLE = {
  codex:  { label: 'CX', color: '#7aa2f7' },
  claude: { label: 'CC', color: '#9ece6a' },
};
const AGENT_DEFAULT = { label: '', color: '#565f89' };

const ui = {
  status: 'thinking',
  text: '正在连接 daemon…',
  session: '',
  agent: '',              // daemon 侧管道标识（"codex-cli"/"claude-code"）
  bubbles: [],
  tokens: null,
  connected: false,
};

/* ---------- 形象素材槽 ---------- */
const IMG_SIZE = 176;
const stateImgs = {};
for (const s of Object.keys(STATE_STYLE)) {
  const img = new Image();
  img.onload = () => { stateImgs[s] = img; };
  img.src = `assets/${s}.png`;
}

/* ---------- 协议客户端 ---------- */

const WS_PORT = 7332;
const DEV_ID = 'virtual-1';

function wsUrl() {
  const host = location.hostname || '127.0.0.1';
  return `ws://${host}:${WS_PORT}`;
}

const sock = { ws: null, backoff: 1000 };

function connect() {
  const ws = new WebSocket(wsUrl());
  sock.ws = ws;

  ws.onopen = () => {
    sock.backoff = 1000;
    send({ ver: 1, type: 'hello', device: DEV_ID, fw: '0.1.0', hw: 'browser', caps: ['button'] });
  };

  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch { return; }
    switch (m.type) {
      case 'hello_ok':
        ui.connected = true;
        setConn(true, `握手成功 ${m.echo}@${m.daemon_ver}`);
        break;
      case 'state':
        ui.status = STATE_STYLE[m.status] ? m.status : 'thinking';
        ui.text = m.text || STATE_STYLE[ui.status].zh;
        ui.session = m.session || '';
        ui.agent = m.agent || '';
        ui.bubbles = Array.isArray(m.bubbles)
          ? m.bubbles.filter((b) => b && STATE_STYLE[b.st]).slice(0, 6)
          : [];
        const t = m.tokens;
        ui.tokens = t && Number.isFinite(t.ctx) && Number.isFinite(t.tot)
          && t.ctx >= 0 && t.tot >= 0 ? { ctx: t.ctx, tot: t.tot } : null;
        break;
      case 'heartbeat':
        if (!m.reply) {
          send({ ver: 1, type: 'heartbeat', uptime_ms: performance.now() | 0, reply: true });
        }
        break;
      default:
        break;
    }
  };

  ws.onclose = () => {
    if (ui.connected) setConn(false, '连接断开，重连中…');
    ui.connected = false;
    setTimeout(connect, sock.backoff);
    sock.backoff = Math.min(sock.backoff * 2, 15000);
  };
  ws.onerror = () => ws.close();
}

function send(obj) {
  if (sock.ws && sock.ws.readyState === WebSocket.OPEN) sock.ws.send(JSON.stringify(obj));
}

function setConn(on, msg) {
  document.getElementById('dot').className = 'dot ' + (on ? 'on' : 'off');
  document.getElementById('foot').textContent = on ? `daemon 已连接 · ${WS_PORT} · ${DEV_ID}` : (msg || '未连接');
}

setInterval(() => send({ ver: 1, type: 'heartbeat', uptime_ms: performance.now() | 0 }), 5000);

/* ---------- YES 键 ---------- */

(() => {
  const btn = document.getElementById('yes');
  btn.addEventListener('pointerup', () => {
    send({ ver: 1, type: 'button', key: 'yes', event: 'short' });
    flashYes();
  });
  let flashT = 0;
  function flashYes() { flashT = performance.now(); }
  window.__yesFlash = () => ({ flashT });
})();

/* ---------- NO 键 ---------- */

(() => {
  const btn = document.getElementById('no');
  btn.addEventListener('pointerdown', () => {
    send({ ver: 1, type: 'button', key: 'no', event: 'short' });
    window.__noFlash = performance.now();
  });
})();

/* ---------- 渲染 ---------- */

const cvs = document.getElementById('screen');
const ctx = cvs.getContext('2d');
const W = 240, H = 320;

function agentStyle(a) {
  if (!a) return AGENT_DEFAULT;
  if (a.startsWith('codex')) return AGENT_STYLE.codex;
  if (a === 'claude-code' || a === 'claude') return AGENT_STYLE.claude;
  return AGENT_DEFAULT;
}

function draw(t) {
  const st = STATE_STYLE[ui.status];
  const cx = W / 2, cy = 150;

  ctx.fillStyle = '#0d0f17';
  ctx.fillRect(0, 0, W, H);

  const grad = ctx.createRadialGradient(cx, cy, 10, cx, cy, 130);
  grad.addColorStop(0, st.color + '2e');
  grad.addColorStop(1, 'transparent');
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, W, H);

  ctx.fillStyle = st.color;
  ctx.fillRect(0, 0, W, 4);
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillStyle = '#565f89';
  ctx.textAlign = 'left';
  ctx.fillText('PETLINK/1', 10, 22);

  // Agent 徽标：标题右侧小色块 + 标签
  const as = agentStyle(ui.agent);
  if (as.label) {
    ctx.font = '700 9px ui-monospace, monospace';
    const tw = ctx.measureText(as.label).width + 10;
    ctx.fillStyle = as.color;
    roundRect(92, 10, tw, 14, 3); ctx.fill();
    ctx.fillStyle = '#ffffff';
    ctx.textAlign = 'center';
    ctx.fillText(as.label, 92 + tw / 2, 21);
    ctx.textAlign = 'left';
  }

  ctx.font = '10px ui-monospace, monospace';
  ctx.fillStyle = '#565f89';
  ctx.textAlign = 'right';
  ctx.fillText(ui.session ? `# ${ui.session.slice(0, 10)}` : '# --', W - 10, 22);

  const n = ui.bubbles.length;
  for (let i = 0; i < n; i++) {
    const b = ui.bubbles[i];
    const foc = !!b.f;
    const r = foc ? 7 : 5;
    const bcx = W - 10 - 8 - (n - 1 - i) * 17;
    const bcy = 38;
    ctx.beginPath();
    ctx.arc(bcx, bcy, r, 0, Math.PI * 2);
    ctx.fillStyle = STATE_STYLE[b.st].color;
    ctx.fill();
    if (foc) {
      ctx.lineWidth = 1.5;
      ctx.strokeStyle = '#ffffff';
      ctx.stroke();
    }
  }

  if (ui.tokens) {
    const fmt = (v) => (v >= 1e6 ? `${(v / 1e6).toFixed(1)}M` : v >= 1e3 ? `${(v / 1e3).toFixed(1)}k` : String(v));
    ctx.font = '10px ui-monospace, monospace';
    ctx.fillStyle = '#565f89';
    ctx.textAlign = 'left';
    ctx.fillText(`ctx ${fmt(ui.tokens.ctx)} / tot ${fmt(ui.tokens.tot)}`, 10, 42);
  }

  const s = ui.status;
  let bob = Math.sin(t / 450) * 4;
  let shake = 0;
  if (s === 'waiting_confirm') bob = Math.abs(Math.sin(t / 160)) * -14;
  if (s === 'error') shake = Math.sin(t / 40) * 3;

  ctx.save();
  ctx.translate(cx + shake, cy + bob);
  const img = stateImgs[s];
  if (img) {
    ctx.drawImage(img, -IMG_SIZE / 2, -IMG_SIZE / 2, IMG_SIZE, IMG_SIZE);
  } else {
    drawDog(t, s, st.color);
  }
  ctx.restore();

  chip(t, st);
  extras(t, s, st.color, cx, cy);

  const f = window.__yesFlash();
  const dt = performance.now() - f.flashT;
  if (dt < 500) {
    ctx.globalAlpha = 1 - dt / 500;
    ctx.textAlign = 'center';
    ctx.font = '700 13px ui-monospace, monospace';
    ctx.fillStyle = '#9ece6a';
    ctx.fillText('YES → ALLOW', cx, H - 40);
    ctx.globalAlpha = 1;
  }
  const ndt = window.__noFlash ? performance.now() - window.__noFlash : 1e9;
  if (ndt < 500) {
    ctx.globalAlpha = 1 - ndt / 500;
    ctx.textAlign = 'center';
    ctx.font = '700 13px ui-monospace, monospace';
    ctx.fillStyle = '#f7768e';
    ctx.fillText('NO → DENY / 切换会话', cx, H - 40);
    ctx.globalAlpha = 1;
  }

  requestAnimationFrame(draw);
}

function drawDog(t, s, color) {
  const r = 52;
  ctx.fillStyle = '#e2e6f3';
  ear(-r * 0.72, -r * 0.55, -0.5);
  ear(r * 0.72, -r * 0.55, 0.5);
  function ear(x, y, dir) {
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(dir);
    ctx.beginPath();
    ctx.ellipse(0, 0, 13, 26, 0, 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();
  }
  ctx.fillStyle = '#f3f6fc';
  ctx.beginPath();
  ctx.arc(0, 0, r, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = '#f5b6c2';
  ctx.beginPath(); ctx.ellipse(-28, 14, 9, 6, 0, 0, Math.PI * 2); ctx.fill();
  ctx.beginPath(); ctx.ellipse(28, 14, 9, 6, 0, 0, Math.PI * 2); ctx.fill();

  ctx.strokeStyle = '#3b4261';
  ctx.fillStyle = '#3b4261';
  ctx.lineWidth = 3.5;
  ctx.lineCap = 'round';
  const eyeY = -8;
  if (s === 'done') {
    happyEye(-18, eyeY); happyEye(18, eyeY);
    ctx.beginPath(); ctx.arc(0, 8, 10, 0.15 * Math.PI, 0.85 * Math.PI); ctx.stroke();
  } else if (s === 'thinking') {
    dot(-18, eyeY - 4); dot(18, eyeY - 4);
    ctx.beginPath(); ctx.arc(0, 10, 5, 0.1 * Math.PI, 0.9 * Math.PI); ctx.stroke();
  } else if (s === 'working') {
    ctx.beginPath(); ctx.moveTo(-25, eyeY - 6); ctx.lineTo(-11, eyeY - 2); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(25, eyeY - 6); ctx.lineTo(11, eyeY - 2); ctx.stroke();
    dot(-18, eyeY + 2); dot(18, eyeY + 2);
    ctx.beginPath(); ctx.moveTo(-7, 12); ctx.lineTo(7, 12); ctx.stroke();
  } else if (s === 'waiting_confirm') {
    dot(-18, eyeY); dot(18, eyeY);
    ctx.beginPath(); ctx.arc(0, 10, 6, 0, Math.PI * 2); ctx.stroke();
  } else if (s === 'error') {
    xEye(-18, eyeY); xEye(18, eyeY);
    ctx.beginPath();
    for (let i = 0; i <= 8; i++) ctx.lineTo(-14 + i * 3.5, 12 + (i % 2 ? 3 : -3));
    ctx.stroke();
    sweat(t);
  }
  function dot(x, y) { ctx.beginPath(); ctx.arc(x, y, 4.2, 0, Math.PI * 2); ctx.fill(); }
  function happyEye(x, y) { ctx.beginPath(); ctx.arc(x, y + 3, 6, 1.1 * Math.PI, 1.9 * Math.PI); ctx.stroke(); }
  function xEye(x, y) {
    ctx.beginPath(); ctx.moveTo(x - 5, y - 5); ctx.lineTo(x + 5, y + 5); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x + 5, y - 5); ctx.lineTo(x - 5, y + 5); ctx.stroke();
  }
  function sweat(t2) {
    ctx.fillStyle = '#7dcfff';
    const y = -30 + Math.sin(t2 / 300) * 3;
    ctx.beginPath();
    ctx.moveTo(38, y);
    ctx.quadraticCurveTo(46, y + 10, 38, y + 14);
    ctx.quadraticCurveTo(30, y + 10, 38, y);
    ctx.fill();
  }
}

function chip(t, st) {
  const label = st.zh;
  ctx.font = '700 13px "PingFang SC", sans-serif';
  const w = ctx.measureText(label).width + 26;
  const x = W / 2 - w / 2, y = 232;
  ctx.fillStyle = st.color;
  roundRect(x, y, w, 24, 12); ctx.fill();
  ctx.fillStyle = '#0d0f17';
  ctx.textAlign = 'center';
  ctx.fillText(label, W / 2, y + 17);

  ctx.font = '11px "PingFang SC", sans-serif';
  ctx.fillStyle = '#9aa5ce';
  ctx.fillText((ui.text || '').slice(0, 16), W / 2, y + 44);
}

function extras(t, s, color, cx, cy) {
  ctx.fillStyle = color;
  if (s === 'thinking') {
    const n = 1 + (Math.floor(t / 350) % 3);
    for (let i = 0; i < n; i++) {
      ctx.beginPath(); ctx.arc(cx - 14 + i * 14, cy - 78, 4, 0, Math.PI * 2); ctx.fill();
    }
  }
  if (s === 'working') {
    ctx.strokeStyle = color;
    ctx.lineWidth = 4;
    ctx.beginPath();
    ctx.arc(cx, cy, 84, t / 300, t / 300 + 1.1);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(cx, cy, 84, t / 300 + Math.PI, t / 300 + Math.PI + 1.1);
    ctx.stroke();
  }
  if (s === 'waiting_confirm') {
    const k = 1 + Math.sin(t / 160) * 0.15;
    ctx.save();
    ctx.translate(cx + 60, cy - 66);
    ctx.scale(k, k);
    ctx.fillStyle = color;
    ctx.beginPath(); ctx.arc(0, 0, 13, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#0d0f17';
    ctx.font = '700 16px ui-monospace, monospace';
    ctx.textAlign = 'center';
    ctx.fillText('!', 0, 6);
    ctx.restore();
  }
  if (s === 'done') {
    for (let i = 0; i < 5; i++) {
      const a = t / 500 + i * (Math.PI * 2 / 5);
      const r2 = 86 + Math.sin(t / 260 + i) * 6;
      star(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2, 3.5);
    }
  }
  function star(x, y, r) {
    ctx.save();
    ctx.translate(x, y);
    ctx.beginPath();
    for (let i = 0; i < 8; i++) {
      const rr = i % 2 ? r : r * 2.4;
      ctx.lineTo(Math.cos(i * Math.PI / 4) * rr, Math.sin(i * Math.PI / 4) * rr);
    }
    ctx.closePath();
    ctx.fill();
    ctx.restore();
  }
}

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

setConn(false, '正在连接 daemon…');
connect();
requestAnimationFrame(draw);
