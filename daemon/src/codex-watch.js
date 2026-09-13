'use strict';

var fs = require("fs");
var path = require("path");

var HOME = process.env.USERPROFILE || "C:/Users/Lenovo";
var ROOT = path.join(HOME, ".codex", "sessions");

var MAP = {
  agent_reasoning: "thinking",
  user_message: "thinking",
  task_started: "thinking",
  function_call: "working",
  function_call_output: "working",
  agent_message: "done",
  task_complete: "done",
};

var TERMINAL = { agent_message: 1, task_complete: 1 };
var DONE_IDLE_MS = 4000;

class CodexWatch {
  constructor(opts) {
    this.reg = opts.registry;
    this.foc = opts.focus;
    this.sta = opts.stats;
    this.voi = opts.voice;
    this.lnk = opts.links;
    this.log = opts.log || function () {};
    this.sid = "codex-cli";
    this.ms = 1500;
    this.cur = "done";
    this._lastEv = "";
    this._lastTs = 0;
    this._tracked = {};
    this._errs = 0;
    this._idleTicks = 0;
  }

  start() {
    this.reg.update(this.sid, "codex-cli", "done", "watching");
    this._timer = setInterval(this._scan.bind(this), this.ms);
    if (this._timer.unref) this._timer.unref();
  }

  stop() { if (this._timer) clearInterval(this._timer); }

  _scan() {
    try {
      var now = Date.now();
      var status = null;
      var reason = "";
      var locked = false;

      // Track ALL session files regardless of age.
      // New sessions get discovered here; existing ones get polled for growth.
      var seen = {};
      try { this._walk(ROOT, seen); } catch (e) {}

      var fp;
      for (fp in seen) {
        var sz = seen[fp];
        var trk = this._tracked[fp];

        if (!trk) {
          // First time seeing this file: start at its current tail
          this._tracked[fp] = { off: sz };
          // silent init
          continue;
        }

        if (sz <= trk.off) continue;
        if (sz < trk.off) trk.off = 0;

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
                if (s === "done") { locked = false; status = s; reason = pt; this._lastTs = now; this._lastEv = pt; }
                else if (!locked || s === "working") { status = s; reason = pt; this._lastTs = now; this._lastEv = pt; if (s === "working") locked = true; }
              }
            } catch (e) {}
          }
        } catch (e) {
          delete this._tracked[fp];
        }
      }

      // Cleanup trackers for files removed from disk
      var keys = Object.keys(this._tracked);
      for (var k = 0; k < keys.length; k++) {
        if (!seen[keys[k]]) {
          try { fs.statSync(keys[k]); } catch (e) { delete this._tracked[keys[k]]; }
        }
      }

      // State transition
      if (status && status !== this.cur) {
        this._idleTicks = 0;
        this._go(status, reason);
      } else if (TERMINAL[this._lastEv] && now - this._lastTs > DONE_IDLE_MS && this.cur !== "done") {
        this._idleTicks++;
        if (this._idleTicks >= 2) {
          this._go("done", "idle");
          this._idleTicks = 0;
        }
      } else {
        this._idleTicks = 0;
      }

    } catch (e) {
      this._errs++;
      if (this._errs <= 3) this.log("[codex-watch] err: " + e.message.slice(0, 80));
    }
  }

  _go(s, txt) {
    if (s === this.cur) return;
    this.cur = s;
    this.log("[codex-watch] => " + s + " (" + txt + ")");
    try { this.reg.update(this.sid, "codex-cli", s, txt); } catch (e) {}
    try { this.sta.status(this.sid, s); } catch (e) {}
    try {
      var f = this.foc.frame(this.sid, "codex-cli", s, txt);
      if (f && this.lnk) { this.voi.announceStatus(f.status, f.text); this.lnk.broadcast(f); }
    } catch (e) {}
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
