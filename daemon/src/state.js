'use strict';
/**
 * 状态归一化 + 会话注册表。
 * 输入：Claude Code hooks 事件（或假状态流）；输出：PetLink 五态。
 */

// hook 事件名 → 五态。映射是架构得分点：新 Agent 只需加一张表。
const HOOK_MAP = {
  SessionStart: 'thinking',
  UserPromptSubmit: 'thinking',
  PreToolUse: 'working',
  PostToolUse: 'working',
  PostToolUseFailure: 'error',
  Stop: 'done',
  StopFailure: 'error',
};

/**
 * 归一化一条 hook 事件。
 * @param {object} body POST /state 的 JSON 体（Claude Code hook payload 的宽松子集）
 * @returns {{status:string,text:string}|null} null=无法归一化，调用方忽略
 */
function normalizeHook(body) {
  if (!body || typeof body !== 'object') return null;
  const event = body.event || body.hook_event_name || '';
  let status = HOOK_MAP[event];

  // Notification 需要语义消歧：permission 类通知 → waiting_confirm
  if (event === 'Notification') {
    const msg = String(body.message || '');
    status = /permission|approve|授权|允许/i.test(msg) ? 'waiting_confirm' : 'waiting_confirm';
    // v1 简化：Notification 一律 waiting_confirm（闲聊类通知后续再细分）
  }
  if (!status) return null;

  // 短标签来源优先级：工具名 > 用户 prompt > 通知 message
  const text = String(body.tool_name || body.prompt || body.message || '').slice(0, 32) || '';
  return { status, text };
}

const SESSION_TTL_MS = 60 * 1000; // done/error 会话保留 60s（对应 v3.0 F02R 语义）

class SessionRegistry {
  constructor() {
    /** @type {Map<string, {agent:string,status:string,text:string,updated:number}>} */
    this.sessions = new Map();
  }

  update(session, agent, status, text) {
    this.sessions.set(session, { agent, status, text: String(text || '').slice(0, 32), updated: Date.now() });
  }

  /** 当前活跃会话 = 最近一次有状态更新的会话 */
  active() {
    this.sweep();
    let best = null;
    for (const [id, s] of this.sessions) {
      if (!best || s.updated > best[1].updated) best = [id, s];
    }
    return best;
  }

  sweep() {
    const now = Date.now();
    for (const [id, s] of this.sessions) {
      if (now - s.updated > SESSION_TTL_MS && (s.status === 'done' || s.status === 'error')) {
        this.sessions.delete(id);
      }
    }
  }

  snapshot() {
    this.sweep();
    return [...this.sessions.entries()].map(([id, s]) => ({ session: id, ...s }));
  }
}

module.exports = { normalizeHook, SessionRegistry };
