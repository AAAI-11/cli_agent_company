'use strict';
/**
 * 语音输入 ASR 适配器：设备 PCM → WAV → whisper-cli → 文本（本地推理，零云依赖）。
 *
 * 依赖（都不入仓库）：
 *   二进制：macOS `brew install whisper-cpp`（/opt/homebrew/bin/whisper-cli）
 *           Windows 用官方预编译包，PETLINK_WHISPER_BIN 指到 whisper-cli.exe
 *   模型  ：daemon/models/ggml-small.bin（466MB，.gitignore 排除）
 *           下载：huggingface.co/ggerganov/whisper.cpp → ggml-small.bin
 *
 * 环境变量：PETLINK_WHISPER_BIN（二进制路径）/ PETLINK_WHISPER_MODEL（模型路径）
 *           PETLINK_WHISPER_LANG（识别语言，默认 zh）
 * 缺依赖：启动后首次调用时告警一次，之后静默返回 null（设备侧不受影响）。
 */

const { execFile } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const MODEL_DEFAULT = path.resolve(__dirname, '..', 'models', 'ggml-small.bin');
let missingWarned = false;

function findBinary(envBin) {
  if (envBin) return envBin;
  const bins = [
    '/opt/homebrew/bin/whisper-cli',
    '/usr/local/bin/whisper-cli',
    ...(process.platform === 'win32'
      ? [path.join(process.env.LOCALAPPDATA || '', 'whisper.cpp', 'whisper-cli.exe')]
      : []),
  ];
  return bins.find((b) => b && fs.existsSync(b)) || null;
}

/** 16bit 单声道 WAV 头（44B，PCM 小端） */
function wavFromPcm(pcm, rate) {
  const h = Buffer.alloc(44);
  h.write('RIFF', 0);
  h.writeUInt32LE(36 + pcm.length, 4);
  h.write('WAVE', 8);
  h.write('fmt ', 12);
  h.writeUInt32LE(16, 16);          // fmt 块长
  h.writeUInt16LE(1, 20);            // PCM
  h.writeUInt16LE(1, 22);           // 单声道
  h.writeUInt32LE(rate, 24);        // 采样率
  h.writeUInt32LE(rate * 2, 28);    // 字节率 = rate * 2 bytes
  h.writeUInt16LE(2, 32);           // 块对齐
  h.writeUInt16LE(16, 34);          // 位深
  h.write('data', 36);
  h.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([h, pcm]);
}

/**
 * 转写一段 PCM。
 * @param {{pcm:Buffer, rate:number, log:Function}} o
 * @returns {Promise<string|null>} 转写文本（首尾空白已剥）；null=缺依赖/失败/空结果
 */
async function transcribe({ pcm, rate, log }) {
  const logfn = log || (() => {});
  const bin = findBinary(process.env.PETLINK_WHISPER_BIN);
  const model = process.env.PETLINK_WHISPER_MODEL || MODEL_DEFAULT;
  if (!bin || !fs.existsSync(model)) {
    if (!missingWarned) {
      missingWarned = true;
      logfn(`[voice-in] whisper 缺依赖，语音输入禁用：bin=${bin || '未找到'} model=${fs.existsSync(model) ? 'ok' : model}`);
    }
    return null;
  }
  const base = path.join(os.tmpdir(), `petlink-asr-${process.pid}-${Date.now() % 100000}`);
  const wav = `${base}.wav`;
  const outPrefix = `${base}`; // -otxt 会生成 <base>.txt
  fs.writeFileSync(wav, wavFromPcm(pcm, rate || 16000));

  const lang = process.env.PETLINK_WHISPER_LANG || 'zh';
  const args = ['-m', model, '-f', wav, '-l', lang, '-t', '4', '-np', '-otxt', '-of', outPrefix];
  try {
    await new Promise((resolve, reject) => {
      execFile(bin, args, { timeout: 90 * 1000, maxBuffer: 4 << 20 }, (err) => (err ? reject(err) : resolve()));
    });
    const txt = fs.readFileSync(`${outPrefix}.txt`, 'utf8').trim();
    return txt || null;
  } catch (e) {
    logfn(`[voice-in] whisper 失败：${e.message}`);
    return null;
  } finally {
    if (process.env.PETLINK_ASR_KEEP === '1') {
      logfn(`[voice-in] PETLINK_ASR_KEEP=1，保留原音：${wav}`);
    } else {
      for (const f of [wav, `${outPrefix}.txt`]) { try { fs.unlinkSync(f); } catch {} }
    }
  }
}

module.exports = { transcribe };
