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

  // Notification 语义消歧：notif（测试 curl）/ notification_type（petlink-hook
  // 转发字段）→ 五态。error 类 → error；permission 类（真值或 message 兜底）→
  // waiting_confirm；idle 等闲聊类不转移状态——2026-09-11 真机：空闲约 60s 后
  // Claude Code 发 "Claude is waiting for your input"，一律吞成 waiting_confirm
  // 会把屏锁死在「等你拍板」（无 permission waiter，按键救不回）
  if (event === 'Notification') {
    const kind = String(body.notif || body.notification_type || '').toLowerCase();
    const msg = String(body.message || '').toLowerCase();
    if (kind.includes('error')) status = 'error';
    else if (kind.includes('permission') || msg.includes('permission')) status = 'waiting_confirm';
    else return null; // idle 等闲聊通知：不改变当前状态
  }
  if (!status) return null;

  // 短标签来源优先级：工具名 > 用户 prompt > 通知 message
  const text = String(body.tool_name || body.prompt || body.message || '').slice(0, 32) || '';
  return { status, text };
}

const SESSION_TTL_MS = 60 * 1000;        // done/error 会话保留 60s（对应 v3.0 F02R 语义）
const ACTIVE_STALE_MS = 10 * 60 * 1000; // thinking/working/waiting_confirm 兜底 TTL：
// 活跃会话每个工具调用都有 hook 刷新 updated，唯一合法长间隔 = 单个长命令执行；
// waiting_confirm 的真授权窗口只有 35s（hooks 超时回落），超过 10 分钟必是
// 粗暴关窗/断杀（无 SessionEnd）——留着只会让气泡永远赖在屏上（09-11 真机）

class SessionRegistry {
  constructor() {
    /** @type {Map<string, {agent:string,status:string,text:string,updated:number}>} */
    this.sessions = new Map();
  }

  update(session, agent, status, text) {
    this.sessions.set(session, { agent, status, text: String(text || '').slice(0, 32), updated: Date.now() });
  }

  /** 显式摘除（SessionEnd）。@returns {boolean} 是否真的摘了 */
  remove(session) {
    return this.sessions.delete(session);
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
      const age = now - s.updated;
      if (age > SESSION_TTL_MS && (s.status === 'done' || s.status === 'error')) {
        this.sessions.delete(id);
      } else if (age > ACTIVE_STALE_MS) {
        this.sessions.delete(id); // 非 done 态兜底：源头已死（关窗/断杀），气泡别赖着
      }
    }
  }

  snapshot() {
    this.sweep();
    return [...this.sessions.entries()].map(([id, s]) => ({ session: id, ...s }));
  }
}

module.exports = { normalizeHook, SessionRegistry };
