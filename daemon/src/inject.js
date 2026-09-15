'use strict';
/**
 * 注入 adapter —— tmux send-keys 路径（宪法 §7：注入双路径收敛于 adapter 接口）。
 * permission 路径见 hooks.js 的 PermissionManager。
 *
 * execFile 直传参数数组（无 shell），文本用 -l 字面量注入，无命令拼接面。
 */

const { execFile } = require('child_process');

function tmux(args) {
  // PETLINK_TMUX_BIN：注入适配器隔离阀（同 WHISPER_BIN 手法）——
  // 测试/沙箱喂假 tmux 脚本，绝不碰真实会话
  const bin = process.env.PETLINK_TMUX_BIN || 'tmux';
  return new Promise((resolve, reject) => {
    execFile(bin, args, { timeout: 3000 }, (err, stdout, stderr) => {
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

/**
 * F17 摇杆导航的 Mac 端视图同步（宪法 v3.5 §4.8）：把焦点会话所在的
 * window/pane 设为其会话的当前视图。select-window 改的是会话级当前窗口
 * （所有附着该会话的终端一起跟），select-pane 再把活动面板落到目标上。
 * best-effort：失败由调用方吞掉，绝不影响焦点切换本身。
 */
async function focusPane(pane) {
  const win = String(pane).replace(/\.\d+$/, ''); // sess:2.1 → sess:2
  await tmux(['select-window', '-t', win]);
  await tmux(['select-pane', '-t', pane]);
}

module.exports = { listPanes, sendText, focusPane };
