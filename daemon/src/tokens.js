'use strict';
/**
 * F15 token 消耗统计（宪法 §4.5）：增量 tail 各会话的 transcript JSONL，
 * 从 assistant 消息的 usage 四项累计。口径：
 *   ctx = 最新一条 usage 的 input+output+cache_creation+cache_read 之和
 *         （此刻上下文占多大、离 compact 还多远——compact 后自然回落）
 *   tot = 历次 usage 四项累加（本会话共烧多少，单调递增）
 * 主线程口径：Task 子代理的 transcript 是独立文件，不计入。
 *
 * 数据源：hook 载荷自带的 transcript_path（旁路只读，零侵入 Claude Code）。
 * 常规 hook 事件不带 usage、Stop 只有金额汇总，所以走流水解析。
 * 长时间回合无 hook 事件时由 server 侧定时轮询兜底（PETLINK_TOKENS_POLL_MS）。
 */

const fs = require('fs');

class TokenTracker {
  /** @param {(m: string) => void} log */
  constructor(log) {
    this.log = log || (() => {});
    /** @type {Map<string, {path:string, offset:number, ctx:number, tot:number}>} */
    this.bySession = new Map();
  }

  /**
   * 增量读 transcript 尾部，更新该会话计数。
   * @param {string} session 会话标识
   * @param {string=} path 本次的 transcript 路径；省略时复用上次存的（轮询路径）
   * @returns {{ctx:number, tot:number}|null} 快照；从未见过且无路径 → null
   */
  poll(session, path) {
    if (!session) return null;
    const st = this.bySession.get(session);
    if (!st) {
      if (!path) return null;
      this.bySession.set(session, { path, offset: 0, ctx: 0, tot: 0 });
      return this._read(session);
    }
    if (path && path !== st.path) {
      // 换文件（compact / 续会）：新文件是新真相，整重扫
      st.path = path; st.offset = 0; st.ctx = 0; st.tot = 0;
    }
    return this._read(session);
  }

  /** 当前快照（供 state 帧组装）；没见过的会话返回 null */
  get(session) {
    const st = this.bySession.get(session);
    return st ? { ctx: st.ctx, tot: st.tot } : null;
  }

  /** 所有会话的快照（/status 调试用） */
  snapshotAll() {
    const out = {};
    for (const [k, st] of this.bySession) out[k] = { ctx: st.ctx, tot: st.tot };
    return out;
  }

  _read(session) {
    const st = this.bySession.get(session);
    let size = 0;
    try { size = fs.statSync(st.path).size; } catch { return this.get(session); } // 文件还没落盘/被清
    if (size < st.offset) { // 变短（rewind/截断）：整重扫
      st.offset = 0; st.ctx = 0; st.tot = 0;
    }
    if (size === st.offset) return this.get(session);
    let fd;
    try {
      fd = fs.openSync(st.path, 'r');
      const buf = Buffer.alloc(size - st.offset);
      fs.readSync(fd, buf, 0, buf.length, st.offset);
      // 只消费到最后一个完整行：半行留到下次（offset 不越过末尾 \n）
      const nl = buf.lastIndexOf(0x0a);
      if (nl === -1) return this.get(session);
      let lineStart = 0;
      for (let i = 0; i <= nl; i++) {
        if (buf[i] === 0x0a) {
          if (i > lineStart) this._line(buf.toString('utf8', lineStart, i), st);
          lineStart = i + 1;
        }
      }
      st.offset += nl + 1;
    } catch (e) {
      this.log(`[tokens] 读 ${st.path} 失败：${e.message}`);
    } finally {
      if (fd !== undefined) try { fs.closeSync(fd); } catch { /* 忽略 */ }
    }
    return this.get(session);
  }

  _line(line, st) {
    if (!line.includes('"usage"')) return; // 快速过滤：绝大多数行不是 assistant
    let m;
    try { m = JSON.parse(line); } catch { return; } // 坏行/半行丢弃，不拖垮后续
    const u = m && m.type === 'assistant' && m.message && m.message.usage;
    if (!u) return;
    const row = (u.input_tokens || 0) + (u.output_tokens || 0)
      + (u.cache_creation_input_tokens || 0) + (u.cache_read_input_tokens || 0);
    st.tot += row;
    st.ctx = row; // 最新一条 = 当前上下文规模（含缓存读写）
  }
}

module.exports = { TokenTracker };
