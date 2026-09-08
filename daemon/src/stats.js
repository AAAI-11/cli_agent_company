'use strict';
/**
 * 会话统计：daemon 生命周期内的全量计数器。
 * 数据源：/state hook 事件、permission 仲裁、设备按键、语音输入。
 * 出口：GET /stats（hooks.js）——演示/结题报告的数字都在这。
 *
 * 设计：纯计数 + 时间累加，无持久化（daemon 是常驻进程，重启清零）。
 * 状态时长：会话状态翻转时结算上一段；未结算的开口段在 snapshot() 时按"至今"估算。
 */

const zero = (keys) => Object.fromEntries(keys.map((k) => [k, 0]));

class Stats {
  constructor() {
    this.startedAt = new Date().toISOString();
    this.hookEvents = zero(['SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PostToolUse',
      'PostToolUseFailure', 'Stop', 'StopFailure', 'Notification', 'other']);
    this.statuses = zero(['thinking', 'working', 'waiting_confirm', 'done', 'error']);
    this.statusMs = zero(['thinking', 'working', 'waiting_confirm', 'done', 'error']);
    this.permission = zero(['requests', 'allow', 'deny', 'none']);
    this.buttons = {}; // "yes:short" → n
    this.voice = zero(['requests', 'ok', 'fail', 'chars']);
    this.sessionsSeen = new Set();
    this.lastText = '';
    this.lastTextAt = null;
    /** @type {Map<string, {status:string, at:number}>} session → 当前开口段 */
    this._open = new Map();
  }

  /** hook 事件计数（POST /state 每条一次，无论是否映射成功） */
  event(name) {
    const k = this.hookEvents[name] !== undefined ? name : 'other';
    this.hookEvents[k]++;
  }

  /** 会话状态更新：结算上一段时长 + 新开口段 + 计数 */
  status(session, status) {
    if (!(status in this.statuses)) return; // 未知状态不计（值可能是合法的 0，用 in 判断）
    const now = Date.now();
    const prev = this._open.get(session);
    if (prev) this.statusMs[prev.status] += now - prev.at;
    this._open.set(session, { status, at: now });
    this.statuses[status]++;
    this.sessionsSeen.add(session);
  }

  permissionOutcome(outcome) {
    this.permission.requests++;
    if (this.permission[outcome] !== undefined) this.permission[outcome]++;
  }

  button(key, event) {
    const k = `${key}:${event}`;
    this.buttons[k] = (this.buttons[k] || 0) + 1;
  }

  /** 语音流水线打点：request（收到音频）→ ok（转写并注入）/ fail（任一环节失败） */
  voiceStart() { this.voice.requests++; }
  voiceOk(text) {
    this.voice.ok++;
    this.voice.chars += [...text].length; // 按字符数（中文友好）
    this.lastText = String(text).slice(0, 200);
    this.lastTextAt = new Date().toISOString();
  }
  voiceFail() { this.voice.fail++; }

  snapshot() {
    const now = Date.now();
    const statusMs = { ...this.statusMs };
    for (const { status, at } of this._open.values()) statusMs[status] += now - at; // 开口段按至今估
    return {
      ok: true,
      started_at: this.startedAt,
      totals: {
        hook_events: Object.values(this.hookEvents).reduce((a, b) => a + b, 0),
        sessions: this.sessionsSeen.size,
        permission_requests: this.permission.requests,
        button_presses: Object.values(this.buttons).reduce((a, b) => a + b, 0),
        voice_inputs: this.voice.ok,
      },
      hook_events: this.hookEvents,
      statuses: this.statuses,
      status_ms: statusMs,
      permission: this.permission,
      buttons: this.buttons,
      voice: { ...this.voice, last_text: this.lastText, last_at: this.lastTextAt },
    };
  }
}

module.exports = { Stats };
