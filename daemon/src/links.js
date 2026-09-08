'use strict';
/**
 * 设备链路层：传输无关的连接管理。
 * - WsLink：浏览器虚拟设备（一帧一消息）
 * - TcpLink：NDJSON 调试回环
 * - UsbCdcLink：MVP 主链路（W1-D4 接入，接口见下）
 *
 * 所有承载收敛到同一 handleFrame()：握手 → 心跳 → 按键路由。
 */

const net = require('net');
const { WebSocketServer } = require('ws');
const proto = require('./protocol');

const HEARTBEAT_TIMEOUT_MS = 90 * 1000; // 浏览器后台标签节流到 ~60s/条，窗口必须覆盖（协议 §3.3）

/** 字节流 → NDJSON 行分帧器（TCP 与 USB CDC 共用） */
class LineFramer {
  constructor(onLine) {
    this.buf = '';
    this.onLine = onLine;
  }
  push(chunk) {
    this.buf += chunk;
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i);
      this.buf = this.buf.slice(i + 1);
      if (line.trim()) this.onLine(line);
    }
    if (this.buf.length > proto.MAX_LINE * 2) this.buf = ''; // 异常堆积保护
  }
}

class LinkManager {
  /**
   * @param {{onButton:(key:string,event:string,dev:string)=>void,
   *          onAudio:(dev:string, a:{id:string,rate:number,pcm:Buffer})=>void,
   *          log:Function}} opts
   */
  constructor(opts) {
    this.onButton = opts.onButton || (() => {});
    this.onAudio = opts.onAudio || (() => {});
    this.log = opts.log || (() => {});
    /** @type {Map<string,object>} deviceId → conn（一实一虚多设备即此 Map 的多entry） */
    this.devices = new Map();
    this._seq = 0;
    this._lastState = null; // 最近一次广播的 state，新设备握手后重放（见 hello 处理）
    this._hbTimer = setInterval(() => this._sweepDead(), HEARTBEAT_TIMEOUT_MS);
    this._hbTimer.unref();
  }

  broadcast(msgObj) {
    const line = proto.make(msgObj.type, msgObj);
    if (line.type === 'state') this._lastState = line; // 记住最近状态，供新设备握手重放
    for (const [id, conn] of this.devices) {
      try {
        conn.send(JSON.stringify(line));
        this.log(`[${conn.label}] TX ${JSON.stringify(line)}`);
      } catch (e) {
        this.log(`[${conn.label}] send失败，摘除: ${e.message}`);
        this.devices.delete(id);
      }
    }
    return this.devices.size;
  }

  /** 统一入口：任何承载收到一条完整帧都走这里 */
  _handleFrame(conn, raw) {
    const r = proto.parseFrame(raw);
    if (!r.ok) {
      conn.send(JSON.stringify(proto.make('error', { code: r.err, msg: 'unparseable frame' })));
      return;
    }
    const msg = r.msg;
    if (msg.type !== 'heartbeat') this.log(`[${conn.label}] RX ${JSON.stringify(msg).slice(0, 200)}`); // 心跳不刷日志

    if (!conn.helloed) {
      if (msg.type !== 'hello') {
        // 孤儿设备：上个 daemon 挂了，它还在 90s 判活窗内、只会发心跳。
        // 回 error 会给它"续命"、close 会把自己拖进 3s 闪断循环（2026-09-02 真机事故）。
        // 静默容忍，等它窗口过期降级重发 hello，≤90s 自愈
        if (!conn._orphanLogged) {
          conn._orphanLogged = true;
          this.log(`[${conn.label}] 首帧非 hello（${msg.type}）：孤儿设备，静默等它重握手（≤90s）`);
        }
        return;
      }
      if (r.versionMismatch) {
        conn.send(JSON.stringify(proto.make('error', { code: 'version_mismatch', msg: `daemon supports ver=${proto.VER}` })));
        conn.close();
        return;
      }
      conn.helloed = true;
      conn.deviceId = String(msg.device || 'unnamed');
      conn.meta = { fw: msg.fw, hw: msg.hw, caps: msg.caps || [] };
      this.devices.set(conn.deviceId, conn);
      conn.send(JSON.stringify(proto.make('hello_ok', { app: 'companion-daemon', daemon_ver: require('../package.json').version, echo: conn.deviceId })));
      this.log(`[${conn.label}] 握手完成 → ${conn.deviceId} (fw=${msg.fw} hw=${msg.hw})`);
      if (this._lastState) { // 空闲期掉线重连的设备立刻看到真实状态，否则卡"正在连接"直到下次会话动静
        conn.send(JSON.stringify(this._lastState));
        this.log(`[${conn.label}] 重放最近状态 → ${conn.deviceId}（${this._lastState.status}）`);
      }
      return;
    }

    switch (msg.type) {
      case 'hello':
        // 设备重发 hello = 它没收到上一份 hello_ok（真机 CDC 刚开口的首写偶发丢失）。
        // 幂等重答让设备侧 5s 重试收敛；设备端重复 hello_ok 不会重复回调（固件单测第 3 组）
        conn.lastSeen = Date.now();
        conn.send(JSON.stringify(proto.make('hello_ok', { app: 'companion-daemon', daemon_ver: require('../package.json').version, echo: conn.deviceId })));
        this.log(`[${conn.label}] 设备重发 hello → 幂等重答 hello_ok（${conn.deviceId}）`);
        return;
      case 'heartbeat':
        conn.lastSeen = Date.now();
        if (!msg.reply) { // 防乒乓：应答带 reply:true，收到应答不再回（协议 §3.3）
          conn.send(JSON.stringify(proto.make('heartbeat', { uptime_ms: process.uptime() * 1000 | 0, reply: true })));
        }
        return;
      case 'button':
        conn.lastSeen = Date.now();
        this.onButton(String(msg.key || ''), String(msg.event || 'short'), conn.deviceId);
        return;
      case 'audio': {
        // 语音输入上行（协议 §3.7）：分块 base64 → 按设备+id 重组 → end 时整段交付
        conn.lastSeen = Date.now();
        this._feedAudio(conn, msg);
        return;
      }
      case 'state':
        // v1 状态只由 daemon 广播；设备发来的 state 忽略（前向兼容，不断连）
        return;
      default:
        return; // hello_ok / error / 未知类型：静默忽略
    }
  }

  /**
   * 音频重组（协议 §3.7）：设备按 ~2.4KB b64 分块发，seq 递增，end:true 收尾。
   * 防泄漏：每设备最多 2 个并发 id、单段 ≤4MB b64、首个分块 30s 未收尾即弃。
   */
  _feedAudio(conn, msg) {
    const id = String(msg.id || '');
    if (!id) return;
    conn._audio = conn._audio || new Map();
    const chunks = conn._audio;
    // 超时清扫：卡死的半段直接弃（设备不会重传，只能丢）
    const now = Date.now();
    for (const [k, v] of chunks) {
      if (now - v.firstAt > 30000) chunks.delete(k);
    }
    let seg = chunks.get(id);
    if (!seg) {
      if (chunks.size >= 2) chunks.delete(chunks.keys().next().value); // FIFO 挤掉最老的
      seg = { b64: '', firstAt: now, rate: 0 };
      chunks.set(id, seg);
    }
    if (msg.rate) seg.rate = msg.rate;
    if (typeof msg.b64 === 'string' && msg.b64) {
      seg.b64 += msg.b64;
      if (seg.b64.length > 4 << 20) { // 4MB 上限 ≈ 3MB PCM ≈ 90s@16k——超出即弃段
        chunks.delete(id);
        this.log(`[${conn.label}] 音频段 ${id} 超 4MB，弃`);
        return;
      }
    }
    if (msg.end) {
      chunks.delete(id);
      if (!seg.b64) return;
      const pcm = Buffer.from(seg.b64, 'base64');
      if (!pcm.length) return;
      this.log(`[${conn.label}] 音频段完成 id=${id} ${pcm.length}B @${seg.rate || 16000}Hz`);
      try {
        this.onAudio(conn.deviceId, { id, rate: seg.rate || 16000, pcm });
      } catch (e) {
        this.log(`[${conn.label}] onAudio 处理异常：${e.message}`);
      }
    }
  }

  _register(conn) {
    conn.label = `${conn.kind}#${++this._seq}`;
    conn.helloed = false;
    conn.lastSeen = Date.now();
    conn.onClose = () => {
      if (conn.deviceId) this.devices.delete(conn.deviceId);
      this.log(`[${conn.label}] 断开${conn.deviceId ? `（${conn.deviceId}）` : ''}`);
    };
  }

  _sweepDead() {
    const now = Date.now();
    for (const [id, conn] of this.devices) {
      if (now - conn.lastSeen > HEARTBEAT_TIMEOUT_MS) {
        this.log(`[${conn.label}] 心跳超时，摘除 ${id}`);
        try { conn.close(); } catch {}
        this.devices.delete(id);
      } else {
        this.log(`[${conn.label}] 心跳正常（lastSeen ${Math.round((now - conn.lastSeen) / 1000)}s 前，${id}）`);
      }
    }
  }

  startWs(port) {
    const wss = new WebSocketServer({ port });
    wss.on('connection', (ws) => {
      const conn = { kind: 'ws' };
      this._register(conn);
      ws.on('message', (data) => this._handleFrame(conn, data.toString()));
      ws.on('close', () => conn.onClose());
      ws.on('error', () => {});
      conn.send = (s) => ws.readyState === ws.OPEN && ws.send(s);
      conn.close = () => ws.close();
    });
    this.log(`WsLink 监听 :${port}（虚拟设备）`);
    return wss;
  }

  startTcp(port) {
    const srv = net.createServer((socket) => {
      const conn = { kind: 'tcp' };
      this._register(conn);
      socket.setEncoding('utf8');
      const framer = new LineFramer((line) => this._handleFrame(conn, line));
      socket.on('data', (c) => framer.push(c));
      socket.on('close', () => conn.onClose());
      socket.on('error', () => {});
      conn.send = (s) => socket.write(s + '\n');
      conn.close = () => socket.destroy();
    });
    srv.listen(port);
    this.log(`TcpLink 监听 :${port}（NDJSON 调试回环）`);
    return srv;
  }

  /**
   * UsbCdcLink：MVP 主链路（宪法 §6 传输承载表第一行）。
   * - 枚举串口，按 VID/厂商识别候选（Espressif 303a / WCH 1a86 / FTDI / CP210x）
   * - 环境变量：PETLINK_SERIAL_PATH 指定口、PETLINK_BAUD 波特率（默认 115200）
   * - 断开/无设备时每 2s 重扫，热插拔即恢复
   */
  startUsb() {
    let SerialPort;
    try {
      ({ SerialPort } = require('serialport'));
    } catch {
      this.log('UsbCdcLink 未启用：缺 serialport 依赖（npm install）');
      return null;
    }
    const wantPath = process.env.PETLINK_SERIAL_PATH;
    // 921600：语音输入上行带宽（16kHz×16bit = 32KB/s PCM，115200 只有 ~11.5KB/s）
    const baud = Number(process.env.PETLINK_BAUD) || 921600;
    const KNOWN_VIDS = new Set(['303a', '1a86', '0403', '10c4']); // espressif / wch / ftdi / cp210x
    const isCandidate = (p) => (
      wantPath ? p.path === wantPath
        : KNOWN_VIDS.has(String(p.vendorId || '').toLowerCase())
          || /espressif|wch|waveshare|ftdi|silicon labs/i.test(p.manufacturer || '')
    );

    let stopped = false;
    let paused = false;
    let timer = null;
    let active = null; // { conn, port, path }
    const schedule = (ms = 2000) => { if (!stopped && !paused && !timer) timer = setTimeout(() => { timer = null; scan(); }, ms); };

    const scan = async () => {
      let ports = [];
      try { ports = await SerialPort.list(); } catch (e) { this.log(`CDC 枚举失败: ${e.message}`); }
      const cands = ports.filter(isCandidate);
      // macOS：同一设备有 cu.(callout)/tty.(dialin) 两个节点。dialin 走 modem 控制语义，
      // 实测开 tty. 节点曾触发芯片复位 + 设备从总线消失；且经 tty. 写出会被静默丢弃
      // （2026-09-04 复现：设备只重发 hello 收不到 hello_ok，daemon 侧 RX 正常）。
      // 而 serialport 的 list() 在本机只报 tty.* 路径——所以按身份选中设备后
      // 一律换开同名 cu. 节点（/dev/tty.X → /dev/cu.X），绝不直接连 dialin。
      let hit = cands[0];
      let openPath = hit ? hit.path : null;
      if (hit && process.platform === 'darwin' && hit.path.startsWith('/dev/tty.')) {
        openPath = '/dev/cu.' + hit.path.slice('/dev/tty.'.length);
      }
      if (!hit) {
        if (!this._cdcQuiet) {
          this.log(`UsbCdcLink 就绪待插：当前 ${ports.length} 个串口无候选（等 ESP32/CH34x），此后静默每 2s 重扫`);
          this._cdcQuiet = true;
        }
        schedule();
        return;
      }
      this._cdcQuiet = false;
      this.log(`UsbCdcLink 连接 ${openPath}${hit.manufacturer ? `（${hit.manufacturer}）` : ''} @${baud}`);

      const conn = { kind: 'cdc' };
      this._register(conn);
      const framer = new LineFramer((line) => this._handleFrame(conn, line));
      // dtr/rts 显式置否：断言这两根线会复位 USB-Serial-JTAG 的芯片（默认值 = 复位 + macOS 掉节点）
      const port = new SerialPort({ path: openPath, baudRate: baud, autoOpen: true, dtr: false, rts: false }, (err) => {
        if (err) { this.log(`CDC 打开失败: ${err.message}`); conn.onClose(); schedule(); }
      });
      port.on('data', (c) => framer.push(c.toString('utf8')));
      port.on('close', () => { active = null; conn.onClose(); this.log(`CDC ${openPath} 断开`); schedule(); });
      port.on('error', (e) => this.log(`CDC 错误: ${e.message}`));
      conn.send = (s) => port.write(s + '\n');
      conn.close = () => port.close();
      active = { conn, port, path: openPath };
    };

    this._usb = {
      stop() { stopped = true; if (timer) clearTimeout(timer); },
      /** 烧录窗口：主动关掉当前串口并停止重扫，把口让给 esptool（否则它 Resource busy） */
      pause: () => {
        paused = true;
        if (timer) { clearTimeout(timer); timer = null; }
        const cur = active;
        active = null;
        if (cur) { try { cur.port.close(); } catch {} }
        this.log(`UsbCdcLink 暂停${cur ? `（已释放 ${cur.path}）` : ''}——烧录窗口，POST /usb/resume 恢复`);
      },
      resume: () => {
        paused = false;
        this.log('UsbCdcLink 恢复扫描');
        schedule(200);
      },
    };
    scan();
    return this._usb;
  }

  /** 烧录窗口控制（HTTP /usb/pause /usb/resume 背后）。@return 链路是否启用 */
  pauseUsb() {
    if (!this._usb) return false;
    this._usb.pause();
    return true;
  }

  resumeUsb() {
    if (!this._usb) return false;
    this._usb.resume();
    return true;
  }
}

module.exports = { LinkManager };
