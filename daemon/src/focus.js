'use strict';
/**
 * F02R 多会话焦点管理（宪法 §4.2 Stretch）。
 * 单屏一次只演一只宠物 = 一个"焦点会话"；本模块决定谁是焦点，并把
 * 广播帧组装成"焦点会话 state + 全员 bubbles 快照"。
 *
 * 规则（按优先级）：
 *  1. waiting_confirm 无条件抢占焦点——多会话的核心价值是不错过任何会话的授权
 *  2. 焦点会话被 sweep（done/error 超 TTL 淘汰）→ 回落到最新活跃会话
 *  3. 空闲态 NO 键循环切换（server.js onButton → focus.cycle()；
 *     有 permission 挂起时按键已被仲裁消费，不会走到切换）
 *
 * 前向兼容：bubbles 是 state 帧的可选字段（协议 §3.4），旧设备按 §1 忽略未知字段；
 * 帧头 session/agent/status/text 永远是焦点会话——旧设备因此天然只演焦点，行为不变。
 */

const MAX_BUBBLES = 6; // 与固件 PL_MAX_BUBBLES、虚拟设备渲染上限三方一致

class FocusManager {
  /**
   * @param {import('./state').SessionRegistry} registry
   * @param {(m: string) => void} log
   * @param {(frame: object) => object=} enrich 帧出口装饰（F15：给焦点会话的帧附 tokens）
   */
  constructor(registry, log, enrich) {
    this.registry = registry;
    this.log = log || (() => {});
    this.enrich = enrich || ((f) => f);
    /** @type {string|null} */
    this.focus = null;
  }

  /** 存活会话列表（sweep 后，最近更新在前） */
  _live() {
    return this.registry.snapshot().sort((a, b) => b.updated - a.updated);
  }

  /** 焦点失效（被 sweep / 从未设置）→ 回落：优先本次事件会话，否则最新活跃 */
  _ensureFocus(incoming) {
    if (this.focus && this._live().some((s) => s.session === this.focus)) return;
    this.focus = incoming || this._live()[0]?.session || null;
  }

  /**
   * 一条会话状态到达（POST /state 归一化后）→ 组装待广播帧。
   * 后台会话的更新不抢屏，但 bubbles 会刷新（设备只变气泡色不变脸）。
   * @returns {object|null} state 帧；null = 无存活会话（理论不可达）
   */
  frame(session, agent, status, text) {
    if (status === 'waiting_confirm' && this.focus !== session) {
      this.log(`[focus] ${session.slice(0, 8)} 等待授权 → 抢占焦点（原 ${String(this.focus || '无').slice(0, 8)}）`);
      this.focus = session;
    }
    this._ensureFocus(session);
    const f = this.focus && this._live().find((s) => s.session === this.focus);
    return f ? this._out(f) : null;
  }

  /**
   * 无新事件、按注册表现状重组焦点帧（F15 的 5s 轮询刷新路径：
   * transcript 在长而 hook 事件未到，token 数也得动）。
   */
  reframe() {
    this._ensureFocus(this.focus);
    const f = this.focus && this._live().find((s) => s.session === this.focus);
    return f ? this._out(f) : null;
  }

  /** 帧组装唯一出口：焦点会话 state + bubbles + enrich 装饰 */
  _out(f) {
    return this.enrich({
      type: 'state', session: f.session, agent: f.agent, status: f.status, text: f.text,
      bubbles: this.bubbles(),
    });
  }

  /**
   * bubbles 快照：最近 MAX_BUBBLES 个存活会话，焦点带 f:1。
   * s 取 session 前 8 字符（仅供显示/调试，设备按 f 标记焦点、不按 s 匹配）。
   */
  bubbles() {
    const live = this._live();
    const list = live.slice(0, MAX_BUBBLES);
    if (this.focus && !list.some((s) => s.session === this.focus)) {
      const foc = live.find((s) => s.session === this.focus);
      if (foc) { list.push(foc); list.shift(); } // 焦点被挤出前列时顶替最老的
    }
    return list.map((s) => ({ s: String(s.session).slice(0, 8), st: s.status, f: s.session === this.focus ? 1 : 0 }));
  }

  /**
   * 空闲态 NO 键：循环切到下一个存活会话（按最近更新排序的环）。
   * @returns {object|null} 新焦点的 state 帧；null = 少于 2 个会话，无事可做
   */
  cycle() {
    const live = this._live().map((s) => s.session);
    if (live.length < 2) return null;
    const i = live.indexOf(this.focus);
    this.focus = live[(i + 1) % live.length];
    const f = this._live().find((s) => s.session === this.focus);
    if (!f) return null;
    this.log(`[focus] NO 键切换焦点 → ${this.focus.slice(0, 8)}（${f.status}）`);
    return this._out(f);
  }
}

module.exports = { FocusManager, MAX_BUBBLES };
