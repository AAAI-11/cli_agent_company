/**
 * PetLink v1 设备端协议核心实现。接口语义见 petlink.h，帧格式见 docs/protocol.md。
 * JSON 解码用 cJSON（components/cjson，MIT）；出站帧全部字段受控，直接 snprintf。
 */
#include "petlink.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

const char *pl_status_name(pl_status_t st)
{
  switch (st) {
    case PL_ST_CONNECTING:       return "connecting";
    case PL_ST_THINKING:         return "thinking";
    case PL_ST_WORKING:          return "working";
    case PL_ST_WAITING_CONFIRM:  return "waiting_confirm";
    case PL_ST_DONE:             return "done";
    case PL_ST_ERROR:            return "error";
    default:                     return "?";
  }
}

/* ---------------- 出站 ---------------- */

static void pl_send(pl_t *pl, const char *line)
{
  pl->cbs.send_line(line, pl->ctx);
}

static void pl_send_hello(pl_t *pl, uint32_t now_ms)
{
  snprintf(pl->out, sizeof(pl->out),
           "{\"ver\":%d,\"type\":\"hello\",\"device\":\"%s\",\"fw\":\"%s\","
           "\"hw\":\"p4-board\",\"caps\":[\"button\",\"mic\"]}",
           PL_VER, pl->device_id, pl->fw);
  pl->last_hello_ms = now_ms;
  pl_send(pl, pl->out);
}

static void pl_send_heartbeat(pl_t *pl, bool reply)
{
  uint32_t up = pl->cbs.uptime_ms ? pl->cbs.uptime_ms(pl->ctx) : 0;
  if (reply) {
    snprintf(pl->out, sizeof(pl->out),
             "{\"ver\":%d,\"type\":\"heartbeat\",\"uptime_ms\":%lu,\"reply\":true}",
             PL_VER, (unsigned long)up);
  } else {
    snprintf(pl->out, sizeof(pl->out),
             "{\"ver\":%d,\"type\":\"heartbeat\",\"uptime_ms\":%lu}",
             PL_VER, (unsigned long)up);
  }
  pl_send(pl, pl->out);
}

void pl_button(pl_t *pl, const char *key, const char *event)
{
  if (!key || !event) return;
  snprintf(pl->out, sizeof(pl->out),
           "{\"ver\":%d,\"type\":\"button\",\"key\":\"%s\",\"event\":\"%s\"}",
           PL_VER, key, event);
  pl_send(pl, pl->out);
}

/* ---------------- 语音音频帧（协议 §3.7） ---------------- */

static const char kB64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** 标准 base64（带补位）。返回写出长度；cap 不足返回 0（不写半截）。 */
static size_t b64_encode(const uint8_t *src, size_t n, char *dst, size_t cap)
{
  size_t need = ((n + 2) / 3) * 4;
  if (need + 1 > cap) return 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)src[i] << 16;
    if (i + 1 < n) v |= (uint32_t)src[i + 1] << 8;
    if (i + 2 < n) v |= src[i + 2];
    dst[o++] = kB64[(v >> 18) & 63];
    dst[o++] = kB64[(v >> 12) & 63];
    dst[o++] = (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
    dst[o++] = (i + 2 < n) ? kB64[v & 63] : '=';
  }
  dst[o] = '\0';
  return o;
}

void pl_audio(pl_t *pl, const char *id, unsigned seq, bool end, unsigned rate,
              const uint8_t *pcm, size_t n)
{
  if (!id || (n == 0 && !end)) return;

  if (end) {
    int hl = snprintf(pl->out_audio, sizeof(pl->out_audio),
                      "{\"ver\":%d,\"type\":\"audio\",\"id\":\"%s\",\"seq\":%u,\"end\":true}",
                      PL_VER, id, seq);
    if (hl > 0) pl_send(pl, pl->out_audio);
    return;
  }

  int hl = snprintf(pl->out_audio, sizeof(pl->out_audio),
                    "{\"ver\":%d,\"type\":\"audio\",\"id\":\"%s\",\"seq\":%u,"
                    "\"rate\":%u,\"end\":false,\"b64\":\"",
                    PL_VER, id, seq, rate);
  if (hl <= 0) return;
  size_t bl = b64_encode(pcm, n, pl->out_audio + hl, sizeof(pl->out_audio) - hl - 3);
  if (bl == 0) return;
  pl->out_audio[hl + bl] = '"';
  pl->out_audio[hl + bl + 1] = '}';
  pl->out_audio[hl + bl + 2] = '\0';
  pl_send(pl, pl->out_audio);
}

void pl_audio_buffer(pl_t *pl, const char *id, unsigned rate, const uint8_t *pcm, size_t n)
{
  if (!pcm || n == 0) return;
  unsigned seq = 0;
  for (size_t off = 0; off < n; off += PL_AUDIO_CHUNK, seq++) {
    size_t chunk = (n - off < PL_AUDIO_CHUNK) ? n - off : PL_AUDIO_CHUNK;
    pl_audio(pl, id, seq, false, rate, pcm + off, chunk);
  }
  pl_audio(pl, id, seq, true, rate, NULL, 0);
}

/* ---------------- 入站分帧 ---------------- */

static void pl_handle_line(pl_t *pl, const char *line, uint32_t now_ms);

void pl_feed(pl_t *pl, const char *bytes, size_t n, uint32_t now_ms)
{
  for (size_t i = 0; i < n; i++) {
    char c = bytes[i];
    if (c == '\n') {
      if (pl->len > 0 && pl->buf[pl->len - 1] == '\r') pl->len--;
      pl->buf[pl->len] = '\0';
      if (pl->len > 0) pl_handle_line(pl, pl->buf, now_ms);
      pl->len = 0;
    } else {
      if (pl->len < PL_MAX_LINE) {
        pl->buf[pl->len++] = c;
      }
    }
  }
}

/* ---------------- 入站分发 ---------------- */

static pl_status_t status_from(const char *s)
{
  if (!s) return PL_ST_CONNECTING;
  if (strcmp(s, "thinking") == 0)         return PL_ST_THINKING;
  if (strcmp(s, "working") == 0)          return PL_ST_WORKING;
  if (strcmp(s, "waiting_confirm") == 0)  return PL_ST_WAITING_CONFIRM;
  if (strcmp(s, "done") == 0)             return PL_ST_DONE;
  if (strcmp(s, "error") == 0)            return PL_ST_ERROR;
  return PL_ST_CONNECTING;
}

/* 提取 agent 标识（最长 PL_AGENT_LEN），回退零串 */
static void agent_from(const cJSON *root, char *out, size_t cap)
{
  out[0] = '\0';
  const cJSON *a = cJSON_GetObjectItem(root, "agent");
  const char *s = cJSON_GetStringValue((cJSON *)a);
  if (s) {
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
  }
}

static void pl_handle_line(pl_t *pl, const char *line, uint32_t now_ms)
{
  cJSON *root = cJSON_Parse(line);
  if (!root) return;

  const cJSON *type = cJSON_GetObjectItem(root, "type");
  const char *t = cJSON_GetStringValue((cJSON *)type);
  if (!t) { cJSON_Delete(root); return; }

  const cJSON *ver = cJSON_GetObjectItem(root, "ver");
  if (cJSON_IsNumber(ver) && (int)ver->valuedouble != PL_VER) {
    cJSON_Delete(root);
    return;
  }

  if (strcmp(t, "hello_ok") == 0) {
    pl->last_rx_ms = now_ms;
    if (!pl->helloed) {
      pl->helloed = true;
      pl->last_hb_ms = now_ms;
      const cJSON *dv = cJSON_GetObjectItem(root, "daemon_ver");
      const char *v = cJSON_GetStringValue((cJSON *)dv);
      if (pl->cbs.on_hello_ok) pl->cbs.on_hello_ok(v ? v : "?" , pl->ctx);
    }
  } else if (strcmp(t, "heartbeat") == 0) {
    pl->last_rx_ms = now_ms;
    const cJSON *reply = cJSON_GetObjectItem(root, "reply");
    if (!cJSON_IsTrue(reply)) {
      pl_send_heartbeat(pl, true);
    }
  } else if (strcmp(t, "state") == 0) {
    pl->last_rx_ms = now_ms;
    const cJSON *st = cJSON_GetObjectItem(root, "status");
    const char *s = cJSON_GetStringValue((cJSON *)st);
    pl_status_t mapped = status_from(s);

    /* 提取 agent 标识 */
    char agent[PL_AGENT_LEN];
    agent_from(root, agent, sizeof(agent));

    /* F02R 多会话气泡 */
    pl->bubble_n = 0;
    if (mapped != PL_ST_CONNECTING) {
      const cJSON *bub = cJSON_GetObjectItem(root, "bubbles");
      const cJSON *e = NULL;
      cJSON_ArrayForEach(e, bub) {
        if (pl->bubble_n >= PL_MAX_BUBBLES) break;
        if (!cJSON_IsObject(e)) continue;
        const cJSON *bst = cJSON_GetObjectItem(e, "st");
        pl_status_t bm = status_from(cJSON_GetStringValue((cJSON *)bst));
        if (bm == PL_ST_CONNECTING) continue;
        const cJSON *bf = cJSON_GetObjectItem(e, "f");
        pl->bubbles[pl->bubble_n].st = bm;
        pl->bubbles[pl->bubble_n].focus = cJSON_IsTrue(bf) ||
            (cJSON_IsNumber(bf) && bf->valuedouble != 0);
        pl->bubble_n++;
      }
    }

    /* F15 token 消耗 */
    pl->tokens_valid = false;
    if (mapped != PL_ST_CONNECTING) {
      const cJSON *tk = cJSON_GetObjectItem(root, "tokens");
      const cJSON *tc = cJSON_GetObjectItem(tk, "ctx");
      const cJSON *tt = cJSON_GetObjectItem(tk, "tot");
      if (cJSON_IsNumber(tc) && cJSON_IsNumber(tt)
          && tc->valuedouble >= 0 && tt->valuedouble >= 0) {
        pl->tokens.ctx = (uint32_t)tc->valuedouble;
        pl->tokens.tot = (uint32_t)tt->valuedouble;
        pl->tokens_valid = true;
      }
    }

    if (mapped != PL_ST_CONNECTING && pl->cbs.on_state) {
      const cJSON *sess = cJSON_GetObjectItem(root, "session");
      const cJSON *text = cJSON_GetObjectItem(root, "text");
      /* agent 已提取，空串表示未携带 */
      pl->cbs.on_state(mapped,
                       cJSON_GetStringValue((cJSON *)sess),
                       agent[0] ? agent : NULL,
                       cJSON_GetStringValue((cJSON *)text),
                       pl->bubbles, pl->bubble_n,
                       pl->tokens_valid ? &pl->tokens : NULL,
                       pl->ctx);
    }
  }

  cJSON_Delete(root);
}

/* ---------------- 生命周期 ---------------- */

void pl_init(pl_t *pl, const pl_cbs_t *cbs, void *ctx,
             const char *device_id, const char *fw)
{
  memset(pl, 0, sizeof(*pl));
  pl->cbs = *cbs;
  pl->ctx = ctx;
  snprintf(pl->device_id, sizeof(pl->device_id), "%s", device_id);
  snprintf(pl->fw, sizeof(pl->fw), "%s", fw);
  pl->helloed = false;
  pl->len = 0;
}

void pl_start(pl_t *pl, uint32_t now_ms)
{
  pl->last_rx_ms = now_ms;
  pl_send_hello(pl, now_ms);
}

void pl_poll(pl_t *pl, uint32_t now_ms)
{
  if (!pl->helloed) {
    if (now_ms - pl->last_hello_ms >= PL_HEARTBEAT_MS) {
      pl_send_hello(pl, now_ms);
    }
    return;
  }

  if (now_ms - pl->last_rx_ms > PL_DEAD_MS) {
    pl->helloed = false;
    pl_send_hello(pl, now_ms);
    return;
  }

  if (now_ms - pl->last_hb_ms >= PL_HEARTBEAT_MS) {
    pl->last_hb_ms = now_ms;
    pl_send_heartbeat(pl, false);
  }
}
