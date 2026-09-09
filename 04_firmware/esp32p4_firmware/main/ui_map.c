/**
 * 状态 → UI 样式映射表（ui_map.h 的实现）。
 * 颜色与 virtual_device/pet.js 的 STATE_STYLE 逐字同源：
 * thinking #7aa2f7 / working #9ece6a / waiting_confirm #e0af68 /
 * done #73daca / error #f7768e；CONNECTING 用 Tokyo Night 注释灰 #565f89。
 */
#include "ui_map.h"

#include <stddef.h>
#include <string.h>

#include "font_text_symbols.h"

/* 枚举顺序变更必须同步本表（petlink.h 的 pl_status_t 是权威序） */
_Static_assert((int)PL_ST_ERROR == 5, "pl_status_t 顺序变更必须同步 ui_map 表");

static const ui_style_t k_map[] = {
  [PL_ST_CONNECTING]      = { 0x565f89, "正在连接", UI_ANM_BLINK,   "等待 daemon 上线" },
  [PL_ST_THINKING]        = { 0x7aa2f7, "思考中",   UI_ANM_DOTS,   "梳理需求边界" },
  [PL_ST_WORKING]         = { 0x9ece6a, "干活中",   UI_ANM_ARC,     "执行 Bash 命令" },
  [PL_ST_WAITING_CONFIRM] = { 0xe0af68, "等你拍板", UI_ANM_BOUNCE,  "请求执行 rm -rf" },
  [PL_ST_DONE]            = { 0x73daca, "完成",     UI_ANM_STARS,  "构建完成了" },
  [PL_ST_ERROR]           = { 0xf7768e, "出错了",   UI_ANM_SHAKE,   "构建失败了" },
};

const ui_style_t *ui_style_for(pl_status_t st) {
  if ((int)st < 0 || (int)st >= (int)(sizeof(k_map) / sizeof(k_map[0])))
    return &k_map[PL_ST_CONNECTING];
  return &k_map[st];
}

/* 取 s 处一个 UTF-8 码点的字节长；续字节不齐/遇 NUL 按单字节容错（不越界读） */
static size_t cp_len(const char *s) {
  const unsigned char c = (unsigned char)s[0];
  size_t l = 1;
  if      ((c & 0xE0) == 0xC0) l = 2;
  else if ((c & 0xF0) == 0xE0) l = 3;
  else if ((c & 0xF8) == 0xF0) l = 4;
  for (size_t k = 1; k < l; k++) {
    if (s[k] == '\0' || ((unsigned char)s[k] & 0xC0) != 0x80) return 1;
  }
  return l;
}

/* 码点是否在 text 字库内：ASCII 由 lv_font_conv 的 -r 全量覆盖；
 * 非 ASCII 查 PET_FONT_TEXT_SYMBOLS（折中集）。缺字显 '?'——豆腐块是
 * 2026-09-09 真机踩的坑：prompt 原文什么都可能来，不能赌字表。
 * 注意别用 ui_map.h 的 PET_FONT_SYMBOLS——那是 font_pet_32 大字标签的集 */
static int glyph_known(const char *s, size_t l) {
  if (l == 1) return 1; /* ASCII（含 '?' 自身，不会被二次替换） */
  if (l != 3) return 0; /* CJK 基本区外（扩展区等）：字库必无 */
  /* PET_FONT_TEXT_SYMBOLS 全是基本区汉字，直接按 UTF-8 字节串线性查（616 字，够快） */
  for (const char *p = PET_FONT_TEXT_SYMBOLS; *p; p += 3) {
    if ((unsigned char)p[0] == (unsigned char)s[0]
        && (unsigned char)p[1] == (unsigned char)s[1]
        && (unsigned char)p[2] == (unsigned char)s[2]) return 1;
  }
  return 0;
}

void ui_text_line(const char *text, pl_status_t st, char *out, size_t cap) {
  if (cap == 0) return;
  const char *src = (text && text[0]) ? text : ui_style_for(st)->label;
  /* 读 i / 写 n 两个游标分开：缺字 3B 换 1B '?'，目标偏移不再等于源偏移 */
  size_t i = 0, n = 0, cps = 0;
  while (src[i] != '\0' && cps < 16 && n < cap) {
    const size_t l = cp_len(src + i);
    if (n + l > cap - 1) break; /* 剩余容量放不下整个字符 */
    if (glyph_known(src + i, l)) {
      memcpy(out + n, src + i, l);
      n += l;
    } else {
      out[n] = '?';
      n += 1;
    }
    i += l;
    cps++;
  }
  out[n] = '\0';
}
