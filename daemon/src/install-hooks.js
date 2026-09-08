'use strict';
/**
 * hooks 安装器：把 companion-daemon 的 hook 幂等合并进 Claude Code settings。
 *
 * 语义（依据官方 hooks 文档，2026-08 核对）：
 *  - hooks 跨 settings 层为追加式合并 → 安装器只需管理"自己那份"：
 *    先摘掉我们旧条目（识别特征见 OUR_*），再挂新条目，别人的 hooks 不动。
 *  - 状态事件用 command hook（sh -c，stdin 收 JSON，async 不阻塞）。
 *  - PermissionRequest 用 http hook：2xx+decision.behavior 表态，空 body=无决策回落弹窗。
 *
 * 用法：
 *   node src/install-hooks.js --dry-run            预览结果，不写盘
 *   node src/install-hooks.js                      安装/更新（写前自动备份）
 *   node src/install-hooks.js --uninstall          摘除我们的条目
 *   node src/install-hooks.js --file /tmp/x.json   指定 settings 文件（测试用）
 */

const fs = require('fs');
const path = require('path');
const os = require('os');

const PERM_URL = 'http://127.0.0.1:7331/permission';
const OUR_CMD_TOKEN = 'petlink-hook'; // 识别特征：command 字符串含此 token 即我们的
const STATE_EVENTS = [
  'SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PostToolUse',
  'PostToolUseFailure', 'Stop', 'Notification',
];

const argv = process.argv.slice(2);
const flag = (n) => argv.includes(n);
const val = (n) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : undefined; };

const SETTINGS = val('--file') || path.join(os.homedir(), '.claude', 'settings.json');
const HOOK_SCRIPT = path.resolve(__dirname, '..', 'hooks', 'petlink-hook.js');

function isOurs(handler) {
  if (!handler || typeof handler !== 'object') return false;
  if (handler.type === 'command') return String(handler.command || '').includes(OUR_CMD_TOKEN);
  if (handler.type === 'http') return String(handler.url || '') === PERM_URL;
  return false;
}

/** 摘掉我们的事件条目（保留同组里别人的 handler；组空则删组；事件空则删事件） */
function stripOurs(settings) {
  let removed = 0;
  const hooks = settings.hooks || {};
  for (const ev of Object.keys(hooks)) {
    if (!Array.isArray(hooks[ev])) continue;
    const groups = hooks[ev]
      .map((g) => {
        if (!g || !Array.isArray(g.hooks)) return g;
        const keep = g.hooks.filter((h) => {
          if (isOurs(h)) { removed++; return false; }
          return true;
        });
        return keep.length === g.hooks.length ? g : { ...g, hooks: keep };
      })
      .filter((g) => !(g && Array.isArray(g.hooks) && g.hooks.length === 0));
    if (groups.length) hooks[ev] = groups;
    else delete hooks[ev];
  }
  if (Object.keys(hooks).length) settings.hooks = hooks;
  else delete settings.hooks;
  return removed;
}

/** 挂上我们的条目 */
function addOurs(settings) {
  settings.hooks = settings.hooks || {};
  const cmd = `node ${JSON.stringify(HOOK_SCRIPT)}`; // sh -c 执行，路径含空格也安全
  for (const ev of STATE_EVENTS) {
    const groups = Array.isArray(settings.hooks[ev]) ? settings.hooks[ev] : (settings.hooks[ev] = []);
    groups.push({ hooks: [{ type: 'command', command: cmd, async: true, timeout: 5 }] });
  }
  const perm = Array.isArray(settings.hooks.PermissionRequest) ? settings.hooks.PermissionRequest : (settings.hooks.PermissionRequest = []);
  perm.push({ hooks: [{ type: 'http', url: PERM_URL, timeout: 35 }] });

  // http hook 的 URL 允许列表（官方安全约束；无此键的版本会忽略，无害）
  if (!Array.isArray(settings.allowedHttpHookUrls) || !settings.allowedHttpHookUrls.includes(PERM_URL)) {
    settings.allowedHttpHookUrls = [...(settings.allowedHttpHookUrls || []), PERM_URL];
  }
}

function main() {
  const mode = flag('--uninstall') ? 'uninstall' : flag('--dry-run') ? 'dry-run' : 'install';
  let settings = {};
  const existed = fs.existsSync(SETTINGS);
  if (existed) {
    try {
      settings = JSON.parse(fs.readFileSync(SETTINGS, 'utf8'));
      if (typeof settings !== 'object' || settings === null) throw new Error('not an object');
    } catch (e) {
      console.error(`✗ ${SETTINGS} 不是合法 JSON，中止（不动你的文件）：${e.message}`);
      process.exit(1);
    }
  }

  const removed = stripOurs(settings);
  if (mode !== 'uninstall') addOurs(settings);

  console.log(`目标文件: ${SETTINGS}${existed ? '' : '（将新建）'}`);
  console.log(`模式: ${mode}｜摘除旧条目: ${removed}｜${mode === 'uninstall' ? '不重挂' : `重挂: ${STATE_EVENTS.length} 个状态事件 + PermissionRequest(http)`}`);

  if (mode === 'dry-run') {
    console.log('—— 预览（未写盘）——');
    console.log(JSON.stringify(settings, null, 2));
    return;
  }

  if (existed && mode === 'install') {
    const bak = `${SETTINGS}.bak-petlink-${Date.now()}`;
    fs.copyFileSync(SETTINGS, bak);
    console.log(`备份: ${bak}`);
  }
  fs.mkdirSync(path.dirname(SETTINGS), { recursive: true });
  fs.writeFileSync(SETTINGS, JSON.stringify(settings, null, 2) + '\n');
  console.log(`✓ 已写入 ${SETTINGS}`);
  console.log('\n验证：新开一个 Claude Code 会话跑任意命令，daemon 日志应出现 [fake]/state 之外的 session。');
}

main();
