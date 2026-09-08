'use strict';
/**
 * PetLink v1 协议编解码（见 docs/protocol.md）
 * NDJSON 帧：一个 JSON 对象一行。WS 承载时一帧一消息。
 */

const VER = 1;
const MAX_LINE = 4096; // 单帧上限 4KB

const TYPES = new Set(['hello', 'hello_ok', 'heartbeat', 'state', 'button', 'error']);
const STATUSES = new Set(['thinking', 'working', 'waiting_confirm', 'done', 'error']);

/** 组一条协议消息 */
function make(type, fields) {
  return Object.assign({ ver: VER, type }, fields || {});
}

/**
 * 解析一行/一帧。
 * @returns {{ok:true,msg:object}|{ok:false,err:string}}
 */
function parseFrame(raw) {
  const line = String(raw).trim();
  if (!line) return { ok: false, err: 'empty' };
  if (line.length > MAX_LINE) return { ok: false, err: 'frame_too_large' };
  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    return { ok: false, err: 'bad_json' };
  }
  if (typeof msg !== 'object' || msg === null || Array.isArray(msg)) {
    return { ok: false, err: 'bad_json' };
  }
  if (msg.ver !== VER) return { ok: true, msg, versionMismatch: true };
  if (typeof msg.type !== 'string' || msg.type.length === 0) return { ok: false, err: 'bad_type' };
  return { ok: true, msg };
}

/**
 * 校验 state 消息的业务字段（daemon 自用；设备端同名实现见固件）。
 * @returns {null|string} null=合法，否则错误码
 */
function checkState(msg) {
  if (!STATUSES.has(msg.status)) return 'unknown_status';
  if (typeof msg.session !== 'string' || !msg.session) return 'bad_session';
  return null;
}

module.exports = { VER, MAX_LINE, TYPES, STATUSES, make, parseFrame, checkState };
