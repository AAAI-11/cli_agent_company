#ifndef PETLINK_H
#define PETLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_VER            1
#define PL_MAX_LINE       4096
#define PL_HEARTBEAT_MS   5000
#define PL_DEAD_MS        90000
#define PL_AUDIO_CHUNK    1800
#define PL_MAX_BUBBLES    6
#define PL_AGENT_LEN      16

typedef enum {
  PL_ST_CONNECTING = 0,
  PL_ST_THINKING,
  PL_ST_WORKING,
  PL_ST_WAITING_CONFIRM,
  PL_ST_DONE,
  PL_ST_ERROR,
} pl_status_t;

const char *pl_status_name(pl_status_t st);

typedef struct {
  pl_status_t st;
  bool focus;
} pl_bubble_t;

typedef struct {
  uint32_t ctx;
  uint32_t tot;
} pl_tokens_t;

typedef struct {
  void (*send_line)(const char *line, void *ctx);
  /** daemon 状态广播；agent 为 source 管道标识（"codex-cli"/"claude-code"/"claude"），可能为 NULL */
  void (*on_state)(pl_status_t st, const char *session, const char *agent,
                   const char *text,
                   const pl_bubble_t *bubbles, uint8_t bubble_n,
                   const pl_tokens_t *tokens, void *ctx);
  void (*on_hello_ok)(const char *daemon_ver, void *ctx);
  uint32_t (*uptime_ms)(void *ctx);
} pl_cbs_t;

typedef struct {
  pl_cbs_t cbs;
  void *ctx;
  char device_id[24];
  char fw[16];

  bool helloed;
  uint32_t last_rx_ms;
  uint32_t last_hb_ms;
  uint32_t last_hello_ms;

  char buf[PL_MAX_LINE * 2];
  size_t len;

  pl_bubble_t bubbles[PL_MAX_BUBBLES];
  uint8_t bubble_n;

  pl_tokens_t tokens;
  bool tokens_valid;

  char out[256];
  char out_audio[PL_MAX_LINE];
} pl_t;

void pl_init(pl_t *pl, const pl_cbs_t *cbs, void *ctx,
             const char *device_id, const char *fw);
void pl_start(pl_t *pl, uint32_t now_ms);
void pl_feed(pl_t *pl, const char *bytes, size_t n, uint32_t now_ms);
void pl_poll(pl_t *pl, uint32_t now_ms);
void pl_button(pl_t *pl, const char *key, const char *event);
void pl_audio(pl_t *pl, const char *id, unsigned seq, bool end, unsigned rate,
              const uint8_t *pcm, size_t n);
void pl_audio_buffer(pl_t *pl, const char *id, unsigned rate, const uint8_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* PETLINK_H */
