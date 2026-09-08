'use strict';
/**
 * 注入 adapter —— tmux send-keys 路径（宪法 §7：注入双路径收敛于 adapter 接口）。
 * permission 路径见 hooks.js 的 PermissionManager。
 *
 * execFile 直传参数数组（无 shell），文本用 -l 字面量注入，无命令拼接面。
 */

const { execFile } = require('child_process');

function tmux(args) {
  return new Promise((resolve, reject) => {
    execFile('tmux', args, { timeout: 3000 }, (err, stdout, stderr) => {
      if (err) { err.stderr = stderr; reject(err); } else resolve(stdout);
    });
  });
}

/** 列出所有会话的 pane：[{pane:'name:0.1', cmd:'bash', cwd}] */
async function listPanes() {
  const out = await tmux(['list-panes', '-a', '-F', '#{session_name}:#{window_index}.#{pane_index}|#{pane_current_command}|#{pane_current_path}']);
  return out.split('\n').filter(Boolean).map((l) => {
    const [pane, cmd, cwd] = l.split('|');
    return { pane, cmd, cwd };
  });
}

/**
 * 向 pane 字面量注入文本（等价于人手键入）。
 * @param {{pane:string, text:string, enter?:boolean, ctrlC?:boolean}} o
 */
async function sendText(o) {
  if (o.ctrlC) await tmux(['send-keys', '-t', o.pane, 'C-c']);
  await tmux(['send-keys', '-t', o.pane, '-l', String(o.text)]);
  if (o.enter !== false) await tmux(['send-keys', '-t', o.pane, 'Enter']);
  return true;
}

module.exports = { listPanes, sendText };
