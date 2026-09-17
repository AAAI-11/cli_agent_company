'use strict';

/**
 * asr-sherpa.js -- sherpa-onnx-node ASR
 * Uses npm sherpa-onnx-node with OfflineRecognizer (transducer model).
 * Model: Zipformer Chinese 14M (4 files in models/sherpa-zh/)
 */

var fs = require('fs');
var path = require('path');

var MODEL_DIR = process.env.PETLINK_SHERPA_MODEL || path.resolve(__dirname, '..', 'models', 'sherpa-zh');
var RECOGNIZER = null;
var SHERPA = null;

// 加载期可用性探测：npm 包 + 模型四件套缺一即不可用。server.js 靠"导出
// transcribe=null"触发 whisper 回退——探测放调用期会让回退链变死代码
// （模块本身总能加载成功）。缺依赖只告警一次，不拖垮 daemon。
function probeAvailable() {
  try { require('sherpa-onnx-node'); } catch (e) { return false; }
  var need = [
    path.join(MODEL_DIR, 'encoder-epoch-99-avg-1.onnx'),
    path.join(MODEL_DIR, 'decoder-epoch-99-avg-1.onnx'),
    path.join(MODEL_DIR, 'joiner-epoch-99-avg-1.onnx'),
    path.join(MODEL_DIR, 'tokens.txt'),
  ];
  for (var i = 0; i < need.length; i++) if (!fs.existsSync(need[i])) return false;
  return true;
}
var AVAILABLE = probeAvailable();

function getRecognizer(logfn) {
  if (RECOGNIZER) return RECOGNIZER;
  try {
    SHERPA = require('sherpa-onnx-node');
    var cfg = {
      modelConfig: {
        transducer: {
          encoder: path.join(MODEL_DIR, 'encoder-epoch-99-avg-1.onnx'),
          decoder: path.join(MODEL_DIR, 'decoder-epoch-99-avg-1.onnx'),
          joiner:  path.join(MODEL_DIR, 'joiner-epoch-99-avg-1.onnx'),
        },
        tokens: path.join(MODEL_DIR, 'tokens.txt'),
      },
    };
    for (var k in cfg.modelConfig.transducer) {
      if (!fs.existsSync(cfg.modelConfig.transducer[k])) throw new Error('missing: ' + cfg.modelConfig.transducer[k]);
    }
    if (!fs.existsSync(cfg.modelConfig.tokens)) throw new Error('missing: ' + cfg.modelConfig.tokens);
    RECOGNIZER = new SHERPA.OfflineRecognizer(cfg);
    if (logfn) logfn('[asr-sherpa] model loaded');
    return RECOGNIZER;
  } catch(e) {
    if (logfn) logfn('[asr-sherpa] unavailable: ' + e.message);
    return null;
  }
}

async function transcribe(opts) {
  var logfn = opts.log || function(){};
  var rec = getRecognizer(logfn);
  if (!rec) return null;
  try {
    var pcm = opts.pcm;
    var rate = opts.rate || 16000;
    var n = pcm.length / 2;
    var samples = new Float32Array(n);
    for (var i = 0; i < n; i++) {
      samples[i] = pcm.readInt16LE(i * 2) / 32768.0;
    }
    var stream = rec.createStream();
    stream.acceptWaveform({ sampleRate: rate, samples: samples });
    rec.decode(stream);
    var r = rec.getResult(stream);
    var text = r && r.text ? r.text.trim() : null;
    return text;
  } catch(e) {
    logfn('[asr-sherpa] decode err: ' + e.message);
    return null;
  }
}

module.exports = { transcribe: AVAILABLE ? transcribe : null };