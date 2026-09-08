'use strict';
/**
 * 语音输出（TTS 播报）：把关键事件念出来——忙碌时不用盯屏幕。
 *
 * 平台实现（自动探测，均可 PETLINK_VOICE_SPEAKER 覆盖）：
 *   darwin  : `say`（zh 语音自动挑一个可用的）
 *   win32   : powershell.exe + System.Speech（SAPI，Windows 自带零依赖）
 *   linux   : espeak-ng（兜底，质量一般）
 *
 * 开关：PETLINK_VOICE=1 才启用（默认关——CI/冒烟/深夜编码不被念叨）。
 * 防刷：3s 冷却 + 同文本 10s 内不重复（working↔thinking 翻转最凶）。
 * 测试：PETLINK_VOICE_DRY=1 只记日志不发声（冒烟测试用）。
 */

const { spawn, execFileSync } = require('child_process');

/* 哪些状态值得念：thinking/working 太吵不念；出错/完成/等确认必须喊人 */
const ANNOUNCE_STATUS = new Set(['waiting_confirm', 'done', 'error']);
const STATUS_PHRASE = {
  waiting_confirm: '需要确认',
  done: '已完成',
  error: '出错了',
};
const PERMISSION_PHRASE = {
  allow: '已放行',
  deny: '已拒绝',
};

function detectImpl(log) {
  if (process.platform === 'darwin') {
    return {
      name: 'say',
      speak: (text, speaker) => {
        const args = speaker ? ['-v', speaker, text] : [text];
        spawn('say', args, { stdio: 'ignore' }).on('error', () => {});
      },
      defaultSpeaker: () => {
        // 挑第一个中文语音（本机实测 Eddy/Flo 可用；老系统是 Tingting）
        try {
          const list = execFileSync('say', ['-v', '?'], { timeout: 3000 }).toString();
          const zh = list.split('\n').find((l) => /zh[-_]CN/i.test(l));
          return zh ? zh.trim().split(/\s+/)[0] : '';
        } catch (e) {
          log(`[voice] 语音列表获取失败：${e.message}`);
          return '';
        }
      },
    };
  }
  if (process.platform === 'win32') {
    return {
      name: 'powershell SAPI',
      speak: (text) => {
        // 单引号转义：PS 字符串里 '' 即一个 '
        const esc = String(text).replace(/'/g, "''");
        const script = `Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak('${esc}')`;
        spawn('powershell.exe', ['-NoProfile', '-Command', script], { stdio: 'ignore' }).on('error', () => {});
      },
      defaultSpeaker: () => '',
    };
  }
  return {
    name: 'espeak-ng',
    speak: (text) => {
      spawn('espeak-ng', ['-v', 'zh', text], { stdio: 'ignore' }).on('error', () => {});
    },
    defaultSpeaker: () => '',
  };
}

class Voice {
  constructor(log) {
    this.log = log || (() => {});
    this.dry = process.env.PETLINK_VOICE_DRY === '1';
    this.enabled = (process.env.PETLINK_VOICE === '1' || this.dry) && !!detectImpl(log);
    this.minGapMs = Number(process.env.PETLINK_VOICE_GAP_MS) || 3000;
    this._impl = this.enabled ? detectImpl(log) : null;
    this._speaker = process.env.PETLINK_VOICE_SPEAKER || '';
    this._lastAt = 0;
    this._lastText = '';
    this._lastStatus = '';
    if (this.enabled) {
      if (!this._speaker) this._speaker = this._impl.defaultSpeaker();
      if (!this.dry) this.log(`[voice] 语音输出开启（${this._impl.name}${this._speaker ? `，${this._speaker}` : ''}）`);
    }
  }

  /** 实际发声（或 dry 模式记日志）。冷却逻辑在 _gate 里。 */
  _speak(text) {
    const now = Date.now();
    if (now - this._lastAt < this.minGapMs) return false;
    if (text === this._lastText && now - this._lastAt < 10000) return false;
    this._lastAt = now;
    this._lastText = text;
    if (this.dry) {
      this.log(`[voice-dry] ${text}`);
      return true;
    }
    this._impl.speak(text, this._speaker);
    this.log(`[voice] ${text}`);
    return true;
  }

  /** 状态播报：只在状态翻转到"值得念"的态时触发；text 可拼进确认播报 */
  announceStatus(status, text) {
    if (!this.enabled || !ANNOUNCE_STATUS.has(status) || status === this._lastStatus) return false;
    this._lastStatus = status; // 翻回同态不重念：working 之间的多个 waiting_confirm 只念一次
    let phrase = STATUS_PHRASE[status] || status;
    if (status === 'waiting_confirm' && text) phrase = `需要确认：${text}`;
    return this._speak(phrase);
  }

  /** permission 结果播报（allow/deny），none（超时）不念——终端自己会弹 */
  announcePermission(outcome) {
    if (!this.enabled) return false;
    const phrase = PERMISSION_PHRASE[outcome];
    if (!phrase) return false;
    return this._speak(phrase);
  }
}

module.exports = { Voice };
