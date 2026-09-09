/**
 * ui_map（状态→样式映射）宿主机单元测试（不依赖 ESP-IDF / LVGL）。
 * 跑法：sh test/run.sh
 * 覆盖：枚举全覆盖、六色互异、标签字节精确（对齐 pet.js）、
 *       标签汉字 ∈ 字体符号集（缺字→屏显 '?' 的提交前拦截）、
 *       越界回退、动画枚举合法且六态互异。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_map.h"
#include "font_text_symbols.h"

static int total, passed, failed;

#define CHECK(cond, name) do { \
  total++; \
  if (cond) { passed++; printf("  ✅ %s\n", name); } \
  else      { failed++; printf("  ❌ %s  (line %d)\n", name, __LINE__); } \
} while (0)

/** 取下一个 UTF-8 码点（容错：非法首字节当单字节）；*i 前移 */
static uint32_t next_cp(const char *s, size_t *i) {
  const unsigned char *u = (const unsigned char *)s;
  uint32_t cp = u[(*i)++];
  if ((cp & 0xE0) == 0xC0 && u[*i]) {
    cp = ((cp & 0x1F) << 6) | (u[(*i)++] & 0x3F);
  } else if ((cp & 0xF0) == 0xE0 && u[*i] && u[*i + 1]) {
    cp = ((cp & 0x0F) << 12) | ((u[(*i)++] & 0x3F) << 6) | (u[(*i)++] & 0x3F);
  }
  return cp;
}

/** s1 的每个字符（码点）是否都出现在 s2 里 */
static int all_chars_in(const char *s1, const char *s2) {
  for (size_t i = 0; s1[i];) {
    uint32_t cp = next_cp(s1, &i);
    int found = 0;
    for (size_t j = 0; s2[j];) {
      if (next_cp(s2, &j) == cp) { found = 1; break; }
    }
    if (!found) return 0;
  }
  return 1;
}

int main(void) {
  printf("== ui_map：状态→样式映射 ==\n");

  /* 1) 枚举全覆盖 */
  {
    const pl_status_t all[] = { PL_ST_CONNECTING, PL_ST_THINKING, PL_ST_WORKING,
                                PL_ST_WAITING_CONFIRM, PL_ST_DONE, PL_ST_ERROR };
    int ok = 1;
    for (size_t k = 0; k < sizeof(all) / sizeof(all[0]); k++)
      if (ui_style_for(all[k]) == NULL) ok = 0;
    CHECK(ok, "六个枚举全部命中表项");
  }

  /* 2) 六色互异（对角比较） */
  {
    const ui_style_t *s[6];
    for (int k = 0; k < 6; k++) s[k] = ui_style_for((pl_status_t)k);
    int ok = 1;
    for (int a = 0; a < 6; a++)
      for (int b = a + 1; b < 6; b++)
        if (s[a]->fg == s[b]->fg) ok = 0;
    CHECK(ok, "六态颜色互异");
  }

  /* 3) 标签字节精确（与 pet.js STATE_STYLE 的 zh 逐字一致） */
  CHECK(strcmp(ui_style_for(PL_ST_THINKING)->label, "思考中") == 0 &&
        strcmp(ui_style_for(PL_ST_WORKING)->label, "干活中") == 0 &&
        strcmp(ui_style_for(PL_ST_WAITING_CONFIRM)->label, "等你拍板") == 0 &&
        strcmp(ui_style_for(PL_ST_DONE)->label, "完成") == 0 &&
        strcmp(ui_style_for(PL_ST_ERROR)->label, "出错了") == 0 &&
        strcmp(ui_style_for(PL_ST_CONNECTING)->label, "正在连接") == 0,
        "六态标签字节精确匹配");

  /* 4) 标签汉字 ⊆ 字体符号集（缺字→屏显 ?，提交前拦截） */
  {
    int ok = 1;
    for (int k = 0; k < 6; k++)
      if (!all_chars_in(ui_style_for((pl_status_t)k)->label, PET_FONT_SYMBOLS)) ok = 0;
    CHECK(ok, "全部标签汉字都在 PET_FONT_SYMBOLS 里");
  }

  /* 5) 越界/非法回退 CONNECTING 项（前向兼容） */
  CHECK(ui_style_for((pl_status_t)99) == ui_style_for(PL_ST_CONNECTING) &&
        ui_style_for((pl_status_t)-1) == ui_style_for(PL_ST_CONNECTING),
        "越界状态回退 CONNECTING 项");

  /* 6) 动画枚举合法且六态主动画互异（每态有独立可辨识的动效） */
  {
    int ok = 1;
    for (int k = 0; k < 6; k++) {
      ui_anim_t a = ui_style_for((pl_status_t)k)->anim;
      if (a < UI_ANM_BLINK || a > UI_ANM_SHAKE) ok = 0;
      for (int b = k + 1; b < 6; b++)
        if (a == ui_style_for((pl_status_t)b)->anim) ok = 0;
    }
    CHECK(ok, "动画枚举合法且六态互异");
  }

  /* 7) SELFTEST 示例 text：非空，且非 ASCII 字符 ⊆ text 字体符号集
   *    （ASCII 由 lv_font_conv 的 -r 0x20-0x7F 覆盖，不走 --symbols） */
  {
    int ok = 1;
    for (int k = 0; k < 6; k++) {
      const ui_style_t *s = ui_style_for((pl_status_t)k);
      if (!s->sample_text || !s->sample_text[0]) { ok = 0; continue; }
      /* 逐码点：非 ASCII 必须能在符号集里找到 */
      for (size_t i = 0; s->sample_text[i];) {
        uint32_t cp = next_cp(s->sample_text, &i);
        if (cp < 0x80) continue;
        size_t j = 0;
        int found = 0;
        while (PET_FONT_TEXT_SYMBOLS[j]) {
          if (next_cp(PET_FONT_TEXT_SYMBOLS, &j) == cp) { found = 1; break; }
        }
        if (!found) { ok = 0; break; }
      }
    }
    CHECK(ok, "六态 sample_text 非空且汉字都在 PET_FONT_TEXT_SYMBOLS 里");
  }

  /* 8) ui_text_line：空/NULL 回退状态中文名（pet.js L67 同口径） */
  {
    char out[64];
    ui_text_line(NULL, PL_ST_THINKING, out, sizeof(out));
    int ok = strcmp(out, "思考中") == 0;
    ui_text_line("", PL_ST_DONE, out, sizeof(out));
    ok = ok && strcmp(out, "完成") == 0;
    CHECK(ok, "空/NULL text 回退状态中文名");
  }

  /* 9) ui_text_line：非空原样保留（≤16 码点） */
  {
    char out[64];
    ui_text_line("Bash", PL_ST_DONE, out, sizeof(out));
    CHECK(strcmp(out, "Bash") == 0, "短 text 原样保留");
  }

  /* 10) ui_text_line：超长截 16 码点且不劈 UTF-8（混入缺字也不影响截断几何） */
  {
    char out[64];
    /* 20 个在库汉字 → 截前 16 个（48 字节，原样） */
    ui_text_line("梳理需求边界请求构建镜像模拟器宠矢素材资源形", PL_ST_DONE, out, sizeof(out));
    size_t n = strlen(out);
    int ok = (n == 48) && (out[0] == (char)0xE6);
    /* 尾字符必须完整：末字节是续字节、且再回溯 2 字节是 0xE6 前导 */
    ok = ok && (((unsigned char)out[n - 1] & 0xC0) == 0x80)
            && (((unsigned char)out[n - 3] & 0xF0) == 0xE0);
    /* ASCII 40 字符 → 截 16（ASCII 无缺字概念） */
    char out2[64];
    ui_text_line("abcdefghijklmnopqrstuvwxyz0123456789abcd", PL_ST_DONE, out2, sizeof(out2));
    ok = ok && (strlen(out2) == 16);
    /* 20 个全缺字 → 16 个 '?'（每码点 1 字节） */
    char out3[64];
    ui_text_line("㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘㵘", PL_ST_DONE, out3, sizeof(out3));
    ok = ok && (strlen(out3) == 16) && (out3[0] == '?') && (out3[15] == '?');
    CHECK(ok, "超长 text 截 16 码点（在库 48B / ASCII 16B / 缺字 16 个?）");
  }

  /* 11) ui_text_line：cap 极小时安全（不越界，NUL 终结） */
  {
    char out[4];
    ui_text_line("梳理需求边界", PL_ST_DONE, out, sizeof(out));
    int ok = (out[3] == '\0') && strlen(out) <= 3;
    /* 容量 3 放不下 3 字节汉字（需 3B + NUL=4）→ 应得空串或已完整字符 */
    ok = ok && (strlen(out) == 0 || strlen(out) == 3);
    ui_text_line("ab", PL_ST_DONE, out, sizeof(out));
    ok = ok && strcmp(out, "ab") == 0;
    CHECK(ok, "小容量下安全截断（含 NUL）");
  }

  /* 12) ui_text_line：缺字回退 '?'（2026-09-09 真机豆腐块事故——prompt 原文
   *     不受字表控制，缺字必须显 ? 而不是画不出来的方块） */
  {
    char out[64];
    /* 㵘(U+3D58, 基本区外) 字库必无 → ?；ASCII 不受影响 */
    ui_text_line("a㵘b", PL_ST_DONE, out, sizeof(out));
    int ok = strcmp(out, "a?b") == 0;
    /* 混排：在库字原样、缺字替换，不拖垮整串 */
    ui_text_line("测试㵘完成", PL_ST_DONE, out, sizeof(out));
    ok = ok && strcmp(out, "测试?完成") == 0;
    /* 全 ASCII 完全不动 */
    ui_text_line("rm -rf /tmp", PL_ST_DONE, out, sizeof(out));
    ok = ok && strcmp(out, "rm -rf /tmp") == 0;
    CHECK(ok, "缺字回退 '?'（不画豆腐块）");
  }

  /* 13) 字表新增字已同步：'验''检' 必须在 PET_FONT_TEXT_SYMBOLS 里
   *     （gen_font_text.py 补词后忘跑 --header 会在这里炸出来） */
  {
    size_t j = 0;
    int yan = 0, jian = 0;
    while (PET_FONT_TEXT_SYMBOLS[j]) {
      const uint32_t cp = next_cp(PET_FONT_TEXT_SYMBOLS, &j);
      if (cp == 0x9A8C) yan = 1; /* 验 */
      if (cp == 0x68C0) jian = 1; /* 检 */
    }
    CHECK(yan && jian, "字表含 验/检（与 gen_font_text.py 同步）");
  }

  printf("\n结果：%d/%d 通过\n", passed, total);
  return failed ? 1 : 0;
}
