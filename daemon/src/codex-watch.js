'use strict';

var fs = require("fs");
var path = require("path");
var os = require("os");

var HOME = os.homedir();
var ROOT = path.join(HOME, ".codex", "sessions");

/*
 * JSONL 事件 → PetLink 五态映射。
 * agent_message 有意不在此表：它代表"agent 完成了本轮回答"，但此时
 * 整个任务可能还在进行（多轮工具调用之间 agent 会反复输出中间摘要）。
 * 只有 task_complete 才是"整个任务彻底结束"的真实信号——2026-09-16
 * 真机：agent 写一行代码就跑一个 function_call→agent_message 交替，
 * 整个循环几十次，每次 agent_message 都翻 done 并触发语音"已完成"。
 */
var MAP = {
  agent_reasoning: "thinking",
  user_message: "thinking",
  task_started: "thinking",
  function_call: "working",
  function_call_output: "working",
  task_complete: "done",
};

/* 真正代表"任务结束"的 terminal 事件——只有 task_complete 一个。
 * agent_message 不在此列，所以 DONE_IDLE_MS 兜底也不会从 agent_message
 * 出发去做 idle→done 跳转。 */
var TERMINAL = { task_complete: 1 };
var DONE_IDLE_MS = 4000;
var SESSION_PREFIX = "codex";

class CodexWatch {
  constructor(opts) {
    this.reg = opts.registry;
    this.foc = opts.focus;
    this.sta = opts.stats;
    this.voi = opts.voice;
    this.lnk = opts.links;
    this.log = opts.log || function () {};
    this.ms = 1500;
    this._tracked = {};
    this._errs = 0;
    this._registered = false;
  }

  start() {
    this._timer = setInterval(this._scan.bind(this), this.ms);
    if (this._timer.unref) this._timer.unref();
  }

  stop() { if (this._timer) clearInterval(this._timer); }

  /** 从 JSONL 路径提取唯一 session ID */
  _sidFromFile(fp) {
    var name = path.basename(fp, ".jsonl");
    var m = name.match(/([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$/);
    if (m) return SESSION_PREFIX + ":" + m[1];
    return SESSION_PREFIX + ":" + name.slice(-16);
  }

  _scan() {
    try {
      var now = Date.now();
      var seen = {};
      try { this._walk(ROOT, seen); } catch (e) {}
      var hasSessions = Object.keys(seen).length > 0;

      if (hasSessions && !this._registered) {
        this._registered = true;
        this.log("[codex-watch] registered after discovering " + Object.keys(seen).length + " file(s) in " + ROOT);
      }

      var fp;
      for (fp in seen) {
        var sz = seen[fp];
        var trk = this._tracked[fp];

        if (!trk) {
          var sid = this._sidFromFile(fp);
          this._tracked[fp] = { off: sz, sid: sid, cur: "done", lastEv: "", lastTs: 0, idleTicks: 0, lastStatus: null };
          continue;
        }

        if (sz <= trk.off) continue;
        if (sz < trk.off) trk.off = 0;

        var status = null;
        var reason = "";
        var locked = false;

        try {
          var fd = fs.openSync(fp, "r");
          var len = Math.min(sz - trk.off, 2 * 1024 * 1024);
          var buf = Buffer.alloc(len);
          fs.readSync(fd, buf, 0, len, trk.off);
          fs.closeSync(fd);

          var lines = buf.toString("utf8").split("\n").filter(Boolean);
          trk.off = sz;

          for (var j = 0; j < lines.length; j++) {
            try {
              var jj = JSON.parse(lines[j]);
              var p = jj.payload || {};
              var pt = p.type || "";
              if (MAP[pt]) {
                var s = MAP[pt];
                if (s === "done") { locked = false; status = s; reason = pt; trk.lastTs = now; trk.lastEv = pt; }
                else if (!locked || s === "working") { status = s; reason = pt; trk.lastTs = now; trk.lastEv = pt; if (s === "working") locked = true; }
              }
            } catch (e) {}
          }
        } catch (e) {
          delete this._tracked[fp];
          continue;
        }

        /* 每文件的独立状态机 */
        if (status && status !== trk.cur) {
          trk.idleTicks = 0;
          trk.cur = status;
          this.log("[codex-watch] " + trk.sid + " => " + status + " (" + reason + ")");
          this.reg.update(trk.sid, "codex-cli", status, reason);
          try { this.sta.status(trk.sid, status); } catch (e) {}
          try {
            var f = this.foc.frame(trk.sid, "codex-cli", status, reason);
            if (f && this.lnk) { this.voi.announceStatus(f.status, f.text); this.lnk.broadcast(f); }
          } catch (e) {}
        } else if (TERMINAL[trk.lastEv] && now - trk.lastTs > DONE_IDLE_MS && trk.cur !== "done") {
          trk.idleTicks++;
          if (trk.idleTicks >= 2) {
            trk.cur = "done";
            this.log("[codex-watch] " + trk.sid + " => done (idle)");
            this.reg.update(trk.sid, "codex-cli", "done", "idle");
            try { this.sta.status(trk.sid, "done"); } catch (e) {}
            try {
              var f = this.foc.frame(trk.sid, "codex-cli", "done", "idle");
              if (f && this.lnk) { this.voi.announceStatus(f.status, f.text); this.lnk.broadcast(f); }
            } catch (e) {}
            trk.idleTicks = 0;
          }
        } else {
          trk.idleTicks = 0;
        }
      }

      /* 文件被删除 → 从注册表摘除会话并刷新设备显示 */
      var keys = Object.keys(this._tracked);
      for (var k = 0; k < keys.length; k++) {
        if (!seen[keys[k]]) {
          try { fs.statSync(keys[k]); } catch (e) {
            var removedSid = this._tracked[keys[k]].sid;
            this.reg.remove(removedSid);
            this.log("[codex-watch] file removed, unregistered: " + removedSid);
            delete this._tracked[keys[k]];
            try {
              var rf = this.foc.reframe();
              if (rf && this.lnk) this.lnk.broadcast(rf);
            } catch (e) {}
          }
        }
      }

    } catch (e) {
      this._errs++;
      if (this._errs <= 3) this.log("[codex-watch] err: " + e.message.slice(0, 80));
    }
  }

  _walk(dir, out) {
    try {
      var items = fs.readdirSync(dir);
      for (var i = 0; i < items.length; i++) {
        var fp = path.join(dir, items[i]);
        try {
          var st = fs.statSync(fp);
          if (st.isDirectory()) { this._walk(fp, out); }
          else if (items[i].endsWith(".jsonl")) { out[fp] = st.size; }
        } catch (e) {}
      }
    } catch (e) {}
  }
}

module.exports = { CodexWatch };
