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

const ui = {
  status: 'thinking',
  text: '正在连接 daemon…',
  session: '',
  bubbles: [], // F02R 多会话气泡：[{s,st,f}]，未知状态过滤、最多 6 个
  connected: false,
};

/* ---------- 形象素材槽 ----------
 * assets/<state>.png 存在即整只替换该状态的宠物（表情画进图里），
 * 缺失则回退到下方矢量小狗。可以一次只换一张。
 * 约定见 assets/README.md —— 与未来固件的 flash 素材同思路，产出可直接迁移。 */
const IMG_SIZE = 176; // 画布中央正方形显示区
const stateImgs = {};
for (const s of Object.keys(STATE_STYLE)) {
  const img = new Image();
  img.onload = () => { stateImgs[s] = img; }; // 404 时静默回退矢量版
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
        ui.bubbles = Array.isArray(m.bubbles)
          ? m.bubbles.filter((b) => b && STATE_STYLE[b.st]).slice(0, 6)
          : [];
        break;
      case 'heartbeat':
        if (!m.reply) { // 防乒乓：应答带 reply:true，收到应答不再回（协议 §3.3）
          send({ ver: 1, type: 'heartbeat', uptime_ms: performance.now() | 0, reply: true });
        }
        break;
      default:
        break; // 前向兼容：未知消息忽略
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

/* 心跳：保活 daemon 侧 lastSeen */
setInterval(() => send({ ver: 1, type: 'heartbeat', uptime_ms: performance.now() | 0 }), 5000);

/* ---------- YES 键：短按=allow 长按=deny ---------- */

(() => {
  const btn = document.getElementById('yes');
  let t0 = 0;
  btn.addEventListener('pointerdown', () => { t0 = performance.now(); });
  btn.addEventListener('pointerup', () => {
    const event = performance.now() - t0 >= 600 ? 'long' : 'short';
    send({ ver: 1, type: 'button', key: 'yes', event });
    flashYes(event);
  });
  let flashT = 0, flashKind = '';
  function flashYes(kind) { flashT = performance.now(); flashKind = kind; }
  window.__yesFlash = () => ({ flashT, flashKind });
})();

/* ---------- NO 键：按下即发（有授权挂起=拒，空闲=切换焦点会话） ---------- */

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

function draw(t) {
  const st = STATE_STYLE[ui.status];
  const cx = W / 2, cy = 150;

  ctx.fillStyle = '#0d0f17';
  ctx.fillRect(0, 0, W, H);

  // 状态光晕
  const grad = ctx.createRadialGradient(cx, cy, 10, cx, cy, 130);
  grad.addColorStop(0, st.color + '2e');
  grad.addColorStop(1, 'transparent');
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, W, H);

  // 顶栏状态条
  ctx.fillStyle = st.color;
  ctx.fillRect(0, 0, W, 4);
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillStyle = '#565f89';
  ctx.textAlign = 'left';
  ctx.fillText('PETLINK/1', 10, 22);
  ctx.textAlign = 'right';
  ctx.fillText(ui.session ? `# ${ui.session.slice(0, 10)}` : '# --', W - 10, 22);

  // F02R 多会话气泡：session 标签下方右对齐一行，状态色圆点、焦点加大加白环
  const n = ui.bubbles.length;
  for (let i = 0; i < n; i++) {
    const b = ui.bubbles[i];
    const foc = !!b.f;
    const r = foc ? 7 : 5;
    const cx = W - 10 - 8 - (n - 1 - i) * 17;
    const cy = 38;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fillStyle = STATE_STYLE[b.st].color;
    ctx.fill();
    if (foc) {
      ctx.lineWidth = 1.5;
      ctx.strokeStyle = '#ffffff';
      ctx.stroke();
    }
  }

  // 宠物（按状态附加动效）
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

  // 状态徽标 + 短标签
  chip(t, st);
  extras(t, s, st.color, cx, cy);

  // 底部按键回显
  const f = window.__yesFlash();
  const dt = performance.now() - f.flashT;
  if (dt < 500) {
    ctx.globalAlpha = 1 - dt / 500;
    ctx.textAlign = 'center';
    ctx.font = '700 13px ui-monospace, monospace';
    ctx.fillStyle = f.flashKind === 'short' ? '#9ece6a' : '#f7768e';
    ctx.fillText(f.flashKind === 'short' ? 'YES → ALLOW' : 'YES(hold) → DENY', cx, H - 40);
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
  // 耳朵
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
  // 头
  ctx.fillStyle = '#f3f6fc';
  ctx.beginPath();
  ctx.arc(0, 0, r, 0, Math.PI * 2);
  ctx.fill();
  // 腮红
  ctx.fillStyle = '#f5b6c2';
  ctx.beginPath(); ctx.ellipse(-28, 14, 9, 6, 0, 0, Math.PI * 2); ctx.fill();
  ctx.beginPath(); ctx.ellipse(28, 14, 9, 6, 0, 0, Math.PI * 2); ctx.fill();

  // 表情（按状态）
  ctx.strokeStyle = '#3b4261';
  ctx.fillStyle = '#3b4261';
  ctx.lineWidth = 3.5;
  ctx.lineCap = 'round';
  const eyeY = -8;
  if (s === 'done') { // ^ ^
    happyEye(-18, eyeY); happyEye(18, eyeY);
    ctx.beginPath(); ctx.arc(0, 8, 10, 0.15 * Math.PI, 0.85 * Math.PI); ctx.stroke();
  } else if (s === 'thinking') { // 眼睛上看 + 小嘴
    dot(-18, eyeY - 4); dot(18, eyeY - 4);
    ctx.beginPath(); ctx.arc(0, 10, 5, 0.1 * Math.PI, 0.9 * Math.PI); ctx.stroke();
  } else if (s === 'working') { // 目光坚定 + 直嘴
    ctx.beginPath(); ctx.moveTo(-25, eyeY - 6); ctx.lineTo(-11, eyeY - 2); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(25, eyeY - 6); ctx.lineTo(11, eyeY - 2); ctx.stroke();
    dot(-18, eyeY + 2); dot(18, eyeY + 2);
    ctx.beginPath(); ctx.moveTo(-7, 12); ctx.lineTo(7, 12); ctx.stroke();
  } else if (s === 'waiting_confirm') { // 圆眼看镜头 + O 嘴
    dot(-18, eyeY); dot(18, eyeY);
    ctx.beginPath(); ctx.arc(0, 10, 6, 0, Math.PI * 2); ctx.stroke();
  } else if (s === 'error') { // > < + 波浪嘴 + 汗滴
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

  // 短标签
  ctx.font = '11px "PingFang SC", sans-serif';
  ctx.fillStyle = '#9aa5ce';
  ctx.fillText((ui.text || '').slice(0, 16), W / 2, y + 44);
}

function extras(t, s, color, cx, cy) {
  ctx.fillStyle = color;
  if (s === 'thinking') { // 头顶 "..." 循环
    const n = 1 + (Math.floor(t / 350) % 3);
    for (let i = 0; i < n; i++) {
      ctx.beginPath(); ctx.arc(cx - 14 + i * 14, cy - 78, 4, 0, Math.PI * 2); ctx.fill();
    }
  }
  if (s === 'working') { // 旋转进度弧
    ctx.strokeStyle = color;
    ctx.lineWidth = 4;
    ctx.beginPath();
    ctx.arc(cx, cy, 84, t / 300, t / 300 + 1.1);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(cx, cy, 84, t / 300 + Math.PI, t / 300 + Math.PI + 1.1);
    ctx.stroke();
  }
  if (s === 'waiting_confirm') { // 感叹号脉冲
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
  if (s === 'done') { // 星光
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

/* ---------- 启动 ---------- */

setConn(false, '正在连接 daemon…');
connect();
requestAnimationFrame(draw);
