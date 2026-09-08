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

  if (end) { /* 收尾帧不带数据 */
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
  if (bl == 0) return; /* 超帧限：调用方按 PL_AUDIO_CHUNK 分块即不会走到 */
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
      /* 完整一行：剥掉可能的 \r，空行跳过 */
      if (pl->len > 0 && pl->buf[pl->len - 1] == '\r') pl->len--;
      pl->buf[pl->len] = '\0';
      if (pl->len > 0) pl_handle_line(pl, pl->buf, now_ms);
      pl->len = 0; /* 换行即重新同步点：垃圾行之后的下一行总是干净的 */
    } else {
      /* 协议 §4：单帧上限 4KB。超限后不再缓冲，静默丢弃直到下一个换行 */
      if (pl->len < PL_MAX_LINE) {
        pl->buf[pl->len++] = c;
      }
    }
  }
}

/* ---------------- 入站分发 ---------------- */

static pl_status_t status_from(const char *s)
{
  if (!s) return PL_ST_CONNECTING; /* 不认识就当没收到，由调用方忽略 */
  if (strcmp(s, "thinking") == 0)         return PL_ST_THINKING;
  if (strcmp(s, "working") == 0)          return PL_ST_WORKING;
  if (strcmp(s, "waiting_confirm") == 0)  return PL_ST_WAITING_CONFIRM;
  if (strcmp(s, "done") == 0)             return PL_ST_DONE;
  if (strcmp(s, "error") == 0)            return PL_ST_ERROR;
  return PL_ST_CONNECTING; /* 未知 status：前向兼容，忽略 */
}

static void pl_handle_line(pl_t *pl, const char *line, uint32_t now_ms)
{
  cJSON *root = cJSON_Parse(line);
  if (!root) return; /* 坏 JSON（含启动日志等噪声）静默丢弃 */

  const cJSON *type = cJSON_GetObjectItem(root, "type");
  const char *t = cJSON_GetStringValue((cJSON *)type);
  if (!t) { cJSON_Delete(root); return; }

  /* ver 校验（协议 §1：设备端 v1 只处理 ver:1，其余忽略） */
  const cJSON *ver = cJSON_GetObjectItem(root, "ver");
  if (cJSON_IsNumber(ver) && (int)ver->valuedouble != PL_VER) {
    cJSON_Delete(root);
    return;
  }

  /* 判活只认携带会话意义的帧：hello_ok / heartbeat / state。
   * error/未知帧不算——2026-09-02 真机事故：daemon 对孤儿设备回 error，
   * 恰好把设备侧 90s 判活窗无限续期，设备永不降级重握手，双方死锁闪断 */
  if (strcmp(t, "hello_ok") == 0) {
    pl->last_rx_ms = now_ms;
    if (!pl->helloed) {
      pl->helloed = true;
      pl->last_hb_ms = now_ms; /* 心跳节拍锚定到握手时刻，避免握手后立刻补发 */
      const cJSON *dv = cJSON_GetObjectItem(root, "daemon_ver");
      const char *v = cJSON_GetStringValue((cJSON *)dv);
      if (pl->cbs.on_hello_ok) pl->cbs.on_hello_ok(v ? v : "?" , pl->ctx);
    }
  } else if (strcmp(t, "heartbeat") == 0) {
    pl->last_rx_ms = now_ms;
    const cJSON *reply = cJSON_GetObjectItem(root, "reply");
    if (!cJSON_IsTrue(reply)) { /* 防乒乓：只应答探测，不应答应答 */
      pl_send_heartbeat(pl, true);
    }
  } else if (strcmp(t, "state") == 0) {
    pl->last_rx_ms = now_ms;
    const cJSON *st = cJSON_GetObjectItem(root, "status");
    const char *s = cJSON_GetStringValue((cJSON *)st);
    pl_status_t mapped = status_from(s);
    if (mapped != PL_ST_CONNECTING && pl->cbs.on_state) {
      const cJSON *sess = cJSON_GetObjectItem(root, "session");
      const cJSON *text = cJSON_GetObjectItem(root, "text");
      pl->cbs.on_state(mapped,
                       cJSON_GetStringValue((cJSON *)sess),
                       cJSON_GetStringValue((cJSON *)text),
                       pl->ctx);
    }
  }
  /* hello / button / error / 未知 type：设备端不关心，静默忽略（前向兼容） */

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
      pl_send_hello(pl, now_ms); /* 握手没确认：每 5s 重发 hello */
    }
    return;
  }

  if (now_ms - pl->last_rx_ms > PL_DEAD_MS) {
    /* 判活窗口超时：重握手（协议 §2 链路死亡由设备侧重连） */
    pl->helloed = false;
    pl_send_hello(pl, now_ms);
    return;
  }

  if (now_ms - pl->last_hb_ms >= PL_HEARTBEAT_MS) {
    pl->last_hb_ms = now_ms;
    pl_send_heartbeat(pl, false);
  }
}
