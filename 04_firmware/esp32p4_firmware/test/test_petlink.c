/**
 * PetLink 设备端核心宿主机单元测试（不依赖 ESP-IDF）。
 * 跑法：sh test/run.sh
 * 覆盖：编码字节精确性、NDJSON 分帧容错、握手、防乒乓（!）、
 *       五态映射、前向兼容（未知 type/status/坏 JSON）、判活重握手。
 */
#include <stdio.h>
#include <string.h>

#include "petlink.h"

static int total, passed, failed;

#define CHECK(cond, name) do { \
  total++; \
  if (cond) { passed++; printf("  ✅ %s\n", name); } \
  else      { failed++; printf("  ❌ %s  (line %d)\n", name, __LINE__); } \
} while (0)

/* ---------- 测试桩 ---------- */

#define MAX_TX 64
static char tx[MAX_TX][PL_MAX_LINE + 8]; /* 语音帧 ~2.5KB，桩缓冲按帧上限开 */
static int tx_n;
static uint32_t fake_uptime_val;

static void reset_tx(void) { tx_n = 0; }
static void on_send(const char *line, void *ctx) {
  (void)ctx;
  if (tx_n < MAX_TX) snprintf(tx[tx_n++], PL_MAX_LINE + 8, "%s", line);
}
static uint32_t on_uptime(void *ctx) { (void)ctx; return fake_uptime_val; }

/** 喂一条完整消息（自动补 \n，长度由 strlen 计——杜绝手写长度出错的坑） */
static void feed(pl_t *pl, const char *line_no_nl, uint32_t now) {
  char b[512];
  snprintf(b, sizeof(b), "%s\n", line_no_nl);
  pl_feed(pl, b, strlen(b), now);
}

static pl_status_t last_st = PL_ST_CONNECTING;
static char last_sess[32] = "", last_text[64] = "";
static pl_bubble_t last_bubbles[PL_MAX_BUBBLES];
static uint8_t last_bubble_n;
static pl_tokens_t last_tokens;
static bool last_tokens_valid;
static int st_n;
static char hello_ok_ver[32] = "";
static int hello_ok_n;

static void on_state(pl_status_t st, const char *session, const char *text,
                     const pl_bubble_t *bubbles, uint8_t bubble_n,
                     const pl_tokens_t *tokens, void *ctx) {
  (void)ctx;
  last_st = st;
  snprintf(last_sess, sizeof(last_sess), "%s", session ? session : "(null)");
  snprintf(last_text, sizeof(last_text), "%s", text ? text : "(null)");
  last_bubble_n = bubble_n;
  if (bubbles && bubble_n) memcpy(last_bubbles, bubbles, bubble_n * sizeof(pl_bubble_t));
  last_tokens_valid = tokens != NULL;
  if (tokens) last_tokens = *tokens;
  st_n++;
}
static void on_hello_ok(const char *ver, void *ctx) {
  (void)ctx;
  snprintf(hello_ok_ver, sizeof(hello_ok_ver), "%s", ver);
  hello_ok_n++;
}

static void pl_setup(pl_t *pl, const char *id) {
  pl_cbs_t cbs = { on_send, on_state, on_hello_ok, on_uptime };
  pl_init(pl, &cbs, NULL, id, "0.1.0");
  reset_tx(); st_n = 0; hello_ok_n = 0; fake_uptime_val = 4242;
  last_st = PL_ST_CONNECTING; last_sess[0] = last_text[0] = '\0';
  last_bubble_n = 0; last_tokens_valid = false;
}

/** 已握手的会话：pl_start + 喂 hello_ok，返回后设备处于握手完成态 */
static void handshake(pl_t *pl, uint32_t t) {
  pl_start(pl, t);
  reset_tx();
  feed(pl, "{\"ver\":1,\"type\":\"hello_ok\",\"app\":\"companion-daemon\","
           "\"daemon_ver\":\"0.2.0\",\"echo\":\"p4-abc123\"}", t + 10);
}

int main(void) {
  pl_t pl;

  printf("== 1. 出站编码字节精确 ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    pl_start(&pl, 0);
    CHECK(tx_n == 1, "pl_start 只发一条 hello");
    CHECK(strcmp(tx[0],
      "{\"ver\":1,\"type\":\"hello\",\"device\":\"p4-abc123\",\"fw\":\"0.1.0\","
      "\"hw\":\"p4-board\",\"caps\":[\"button\",\"mic\"]}") == 0, "hello 帧逐字节正确");
    reset_tx();
    pl_button(&pl, "yes", "short");
    CHECK(strcmp(tx[0], "{\"ver\":1,\"type\":\"button\",\"key\":\"yes\",\"event\":\"short\"}") == 0,
          "button 帧逐字节正确");
  }

  printf("== 2. NDJSON 分帧：跨包拆分 / 一包多行 / CR 剥离 / 空行 ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    reset_tx(); st_n = 0;
    const char *a = "{\"ver\":1,\"type\":\"state\",\"session\":\"s1\",\"status\":\"working\",\"text\":\"E";
    const char *b = "dit(x)\"}\r\n{\"ver\":1,\"type\":\"state\",\"session\":\"s2\",\"status\":\"done\"}\n\n";
    pl_feed(&pl, a, strlen(a), 100); /* 前半行，无换行 → 不触发 */
    CHECK(st_n == 0, "半行不触发回调");
    pl_feed(&pl, b, strlen(b), 110); /* 后半行 + 第二行 + 空行 */
    CHECK(st_n == 2, "补齐半行 + 一包多行共 2 帧");
    CHECK(last_st == PL_ST_DONE && strcmp(last_sess, "s2") == 0, "最后一帧生效（done/s2）");
  }

  printf("== 3. 握手 ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    CHECK(hello_ok_n == 1 && strcmp(hello_ok_ver, "0.2.0") == 0, "hello_ok 回调带版本号");
    reset_tx();
    feed(&pl, "{\"ver\":1,\"type\":\"hello_ok\",\"daemon_ver\":\"0.2.0\"}", 50);
    CHECK(hello_ok_n == 1, "重复 hello_ok 不重复回调");
  }

  printf("== 4. 防乒乓（daemon 侧事故的回归用例） ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    reset_tx();
    feed(&pl, "{\"ver\":1,\"type\":\"heartbeat\",\"uptime_ms\":999}", 100);
    CHECK(tx_n == 1, "收到探测：恰好回一条");
    CHECK(tx_n == 1 && strstr(tx[0], "\"reply\":true") != NULL && strstr(tx[0], "\"uptime_ms\":4242") != NULL,
          "应答带 reply:true 和本机 uptime");
    reset_tx();
    feed(&pl, "{\"ver\":1,\"type\":\"heartbeat\",\"uptime_ms\":1000,\"reply\":true}", 200);
    CHECK(tx_n == 0, "收到应答：绝不回（否则风暴）");
  }

  printf("== 5. 五态映射与字段透传 ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    const char *sts[] = { "thinking", "working", "waiting_confirm", "done", "error" };
    const pl_status_t want[] = { PL_ST_THINKING, PL_ST_WORKING, PL_ST_WAITING_CONFIRM, PL_ST_DONE, PL_ST_ERROR };
    char line[160];
    for (int i = 0; i < 5; i++) {
      snprintf(line, sizeof(line),
               "{\"ver\":1,\"type\":\"state\",\"session\":\"sx\",\"status\":\"%s\",\"text\":\"t%d\"}",
               sts[i], i);
      feed(&pl, line, 100 + i);
      snprintf(line, sizeof(line), "status=%s 映射正确", sts[i]);
      CHECK(last_st == want[i], line);
    }
    CHECK(strcmp(last_text, "t4") == 0 && strcmp(last_sess, "sx") == 0, "session/text 透传");
    st_n = 0;
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"s\",\"status\":\"hologram\"}", 200);
    CHECK(st_n == 0, "未知 status 前向兼容忽略");
  }

  printf("== 6. 容错：坏 JSON / 日志噪声 / 版本不匹配 / 未知 type ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    reset_tx();
    feed(&pl, "I (234) boot: chip start", 300);
    feed(&pl, "{\"broken", 301);
    feed(&pl, "{\"ver\":9,\"type\":\"state\",\"session\":\"s\",\"status\":\"done\"}", 302);
    feed(&pl, "{\"ver\":1,\"type\":\"mystery\",\"wow\":1}", 303);
    CHECK(tx_n == 0 && st_n == 0, "全部静默忽略且不崩");
    CHECK(strcmp(pl_status_name(PL_ST_WAITING_CONFIRM), "waiting_confirm") == 0,
          "pl_status_name 供 UI 使用");
  }

  printf("== 7. 轮询：握手重试 / 心跳周期 / 判活重握手 ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    pl_start(&pl, 0); /* t=0 发 hello，无人应答 */
    reset_tx();
    pl_poll(&pl, 4000);  CHECK(tx_n == 0, "4s：未到重试点不重发");
    pl_poll(&pl, 5000);  CHECK(tx_n == 1 && strstr(tx[0], "\"hello\""), "5s：重发 hello");
    feed(&pl, "{\"ver\":1,\"type\":\"hello_ok\",\"daemon_ver\":\"0.2.0\"}", 5100);
    reset_tx();
    pl_poll(&pl, 5200);  CHECK(tx_n == 0, "刚握手不立即补心跳");
    pl_poll(&pl, 10200); CHECK(tx_n == 1 && strstr(tx[0], "\"heartbeat\"") && !strstr(tx[0], "reply"),
                               "握手后 5s：发探测（不带 reply）");
    /* 静默 91s → 判活超时 → 重握手 */
    feed(&pl, "{\"ver\":1,\"type\":\"heartbeat\",\"uptime_ms\":1,\"reply\":true}", 11000);
    reset_tx();
    pl_poll(&pl, 11000 + 91000);
    CHECK(tx_n == 1 && strstr(tx[0], "\"hello\""), "静默超 90s：降级重发 hello");
  }

  printf("== 8. 行边界重新同步（超长垃圾行冲刷） ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    char junk[300];
    memset(junk, 'x', sizeof(junk) - 1); junk[sizeof(junk) - 1] = '\0';
    for (int i = 0; i < 60; i++) pl_feed(&pl, junk, strlen(junk), 100); /* ~18KB 无 \n 超限 */
    pl_feed(&pl, "\n", 1, 150);                                          /* 垃圾行冲刷 */
    st_n = 0;
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"s\",\"status\":\"done\"}", 200);
    CHECK(st_n == 1 && last_st == PL_ST_DONE, "垃圾行冲刷后链路立即恢复");
    CHECK(pl.len == 0, "缓冲不残留");
  }

  printf("== 9. 语音音频帧（协议 §3.7） ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);
    reset_tx();

    /* 单帧字节精确：3 字节 PCM 的 b64 = AAEC（手算基准） */
    const uint8_t pcm1[3] = { 0x00, 0x01, 0x02 };
    pl_audio(&pl, "a1", 0, false, 16000, pcm1, sizeof pcm1);
    CHECK(tx_n == 1 && strcmp(tx[0],
      "{\"ver\":1,\"type\":\"audio\",\"id\":\"a1\",\"seq\":0,\"rate\":16000,"
      "\"end\":false,\"b64\":\"AAEC\"}") == 0, "audio 数据帧逐字节正确（b64=AAEC）");

    /* 收尾帧 */
    pl_audio(&pl, "a1", 1, true, 16000, NULL, 0);
    CHECK(tx_n == 2 && strcmp(tx[1],
      "{\"ver\":1,\"type\":\"audio\",\"id\":\"a1\",\"seq\":1,\"end\":true}") == 0,
      "audio 收尾帧逐字节正确");

    /* 整段分块：5000B → 1800+1800+1400 三块 + 收尾 = 4 帧 */
    reset_tx();
    static uint8_t big[5000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7);
    pl_audio_buffer(&pl, "a2", 16000, big, sizeof big);
    CHECK(tx_n == 4, "5000B 整段 → 3 数据帧 + 1 收尾帧");
    int all_in_limit = 1;
    for (int i = 0; i < 4; i++) if (strlen(tx[i]) >= PL_MAX_LINE) all_in_limit = 0;
    CHECK(all_in_limit, "每帧都在 4KB 帧限内");
    /* 块大小：b64 长度 = 4*ceil(n/3)——1800→2400，1400→1868 */
    const char *b64_0 = strstr(tx[0], "\"b64\":\"") + 7;
    const char *b64_2 = strstr(tx[2], "\"b64\":\"") + 7;
    CHECK(b64_0 - tx[0] > 0 && strcspn(b64_0, "\"") == 2400, "第 1 块 b64=2400 字符（1800B）");
    CHECK(b64_2 - tx[2] > 0 && strcspn(b64_2, "\"") == 1868, "第 3 块 b64=1868 字符（1400B）");
    CHECK(strstr(tx[2], "\"seq\":2") != NULL && strstr(tx[3], "\"seq\":3") != NULL &&
          strstr(tx[3], "\"end\":true") != NULL, "seq 连续且收尾帧带 end:true");
    /* 边界防御：n=0 且非 end（非法调用）静默丢弃不崩 */
    reset_tx();
    pl_audio(&pl, "a3", 0, false, 16000, NULL, 0);
    CHECK(tx_n == 0, "n=0 非 end 的非法调用静默忽略");
  }

  printf("== 10. F02R 多会话气泡（协议 §3.4 可选 bubbles） ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);

    /* 完整解析：三项，中间项是焦点 */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"working\","
              "\"bubbles\":[{\"s\":\"aaa\",\"st\":\"working\",\"f\":1},"
              "{\"s\":\"bbb\",\"st\":\"waiting_confirm\",\"f\":0},"
              "{\"s\":\"ccc\",\"st\":\"thinking\"}]}", 100);
    CHECK(st_n == 1 && last_bubble_n == 3, "bubbles 三项全解析");
    CHECK(last_bubbles[0].st == PL_ST_WORKING && last_bubbles[0].focus,
          "焦点项 st/f 正确（缺省 f 视为 false）");
    CHECK(last_bubbles[1].st == PL_ST_WAITING_CONFIRM && !last_bubbles[1].focus,
          "非焦点项 st/f 正确");

    /* 帧不携带 bubbles → 清空（旧 daemon / 回退路径） */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"thinking\"}", 110);
    CHECK(st_n == 2 && last_bubble_n == 0, "未携带 bubbles → bubble_n=0");

    /* 坏项跳过不拖垮整帧：未知 st / 非对象项 */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"done\","
              "\"bubbles\":[{\"st\":\"hologram\"},7,{\"st\":\"error\",\"f\":1}]}", 120);
    CHECK(st_n == 3 && last_bubble_n == 1 && last_bubbles[0].st == PL_ST_ERROR,
          "未知 st/非对象项跳过，好项保留");

    /* 超 6 项截断 */
    char big[512];
    snprintf(big, sizeof(big), "{\"ver\":1,\"type\":\"state\",\"session\":\"m\",\"status\":\"working\","
             "\"bubbles\":[{\"st\":\"done\"},{\"st\":\"done\"},{\"st\":\"done\"},"
             "{\"st\":\"done\"},{\"st\":\"done\"},{\"st\":\"done\"},{\"st\":\"error\"}]}");
    feed(&pl, big, 130);
    CHECK(st_n == 4 && last_bubble_n == PL_MAX_BUBBLES, "超上限截断到 6");
  }

  printf("== 11. F15 token 消耗（协议 §3.4 可选 tokens） ==\n");
  {
    pl_setup(&pl, "p4-abc123");
    handshake(&pl, 0);

    /* 携带 tokens：两数全解析 */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"working\","
              "\"tokens\":{\"ctx\":45123,\"tot\":1234567}}", 100);
    CHECK(st_n == 1 && last_tokens_valid
              && last_tokens.ctx == 45123 && last_tokens.tot == 1234567,
          "tokens ctx/tot 解析");

    /* 未携带 → 无效（UI 隐藏，不用半截数字） */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"thinking\"}", 110);
    CHECK(st_n == 2 && !last_tokens_valid, "未携带 tokens → 无效");

    /* 字段非法（字符串/缺 ctx）：整体无效，不出现半截快照 */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"done\","
              "\"tokens\":{\"ctx\":\"many\",\"tot\":5}}", 120);
    CHECK(st_n == 3 && !last_tokens_valid, "非法 tokens → 整体无效");

    /* 恢复正常后再次生效（无效态不粘住） */
    feed(&pl, "{\"ver\":1,\"type\":\"state\",\"session\":\"aaa\",\"status\":\"error\","
              "\"tokens\":{\"ctx\":715,\"tot\":1935}}", 130);
    CHECK(st_n == 4 && last_tokens_valid && last_tokens.ctx == 715 && last_tokens.tot == 1935,
          "无效后恢复正常解析");
  }

  printf("\n结果：%d/%d 通过\n", passed, total);
  return failed ? 1 : 0;
}
