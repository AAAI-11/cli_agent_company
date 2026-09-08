#!/usr/bin/env python3
"""生成五态测试 PNG（素材槽联调用，非正式美术）。

规格与 virtual_device/assets/README.md 一致：352x352、背景透明、
主体居中占满。正式素材由美术替换，本脚本只保证"有图可烧"。

纯标准库（zlib/struct），不依赖 PIL——干净室工具链最小化依赖。

用法：
  python3 tools/gen_test_pngs.py [输出目录]   # 缺省 tools/assets_png
"""

import os
import struct
import sys
import zlib

SIZE = 352
CX = CY = SIZE // 2
BODY = 140          # 主体半径（占满 352 的 80%）

# 五态基色（RGBA 前景，背景恒透明）——浏览器虚拟设备同款色系
STATES = {
    "thinking":        (0x5b, 0x8d, 0xef),
    "working":         (0x3f, 0xc1, 0x6b),
    "waiting_confirm": (0xf0, 0xa0, 0x30),
    "done":            (0x2e, 0xc4, 0xb0),
    "error":           (0xe5, 0x55, 0x55),
}


def write_png(path, rgba_rows):
    """按 PNG 规范手写文件：IHDR + IDAT(zlib) + IEND。"""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + row for row in rgba_rows)  # 每行 filter=0
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def circle(cx, cy, r, fill, rows, alpha=255):
    for y in range(max(0, cy - r), min(SIZE, cy + r)):
        dy2 = (y - cy) ** 2
        span = int((r * r - dy2) ** 0.5)
        for x in range(max(0, cx - span), min(SIZE, cx + span)):
            rows[y][x * 4:x * 4 + 4] = bytes((*fill, alpha))


def dot(cx, cy, r, color, rows):
    """小实心圆（眼睛/腮红），color 可为 None=挖空（X 眼的笔画）。"""
    if color is None:
        for y in range(max(0, cy - r), min(SIZE, cy + r)):
            dy2 = (y - cy) ** 2
            span = int((r * r - dy2) ** 0.5)
            for x in range(max(0, cx - span), min(SIZE, cx + span)):
                rows[y][x * 4:x * 4 + 3] = bytes((0, 0, 0))
        return
    circle(cx, cy, r, color, rows)


def mouth(cx, cy, w, h, rows, smile=True):
    """简单弧线嘴：沿圆弧描点。"""
    n = 24
    for i in range(n + 1):
        t = i / n
        x = int(cx - w / 2 + w * t)
        y = int(cy + (h * (1 - (2 * t - 1) ** 2)) ** 0.5 * (1 if smile else -1))
        if 0 <= x < SIZE and 0 <= y < SIZE:
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    if dx * dx + dy * dy <= 5 and 0 <= x + dx < SIZE and 0 <= y + dy < SIZE:
                        rows[y + dy][(x + dx) * 4:(x + dx) * 4 + 3] = bytes((30, 30, 40))


def gen_state(name, rgb):
    rows = [bytearray(SIZE * 4) for _ in range(SIZE)]
    circle(CX, CY, BODY, rgb, rows)                       # 身体
    circle(CX, CY - BODY - 8, 64, rgb, rows, alpha=70)    # 头顶淡光环（区分上下）
    dark = (30, 30, 40)
    if name == "thinking":
        dot(CX - 48, CY - 30, 12, dark, rows)             # 双眼看右上
        dot(CX + 48, CY - 30, 12, dark, rows)
        for i, (dx, dy, r) in enumerate([(70, -80, 6), (95, -100, 8), (124, -116, 11)]):
            dot(CX + dx, CY + dy, r, dark, rows)          # 思考泡泡
    elif name == "working":
        dot(CX - 48, CY - 30, 12, dark, rows)             # 平视专注
        dot(CX + 48, CY - 30, 12, dark, rows)
        dot(CX - 70, CY + 10, 10, (255, 200, 200), rows)  # 腮红
        dot(CX + 70, CY + 10, 10, (255, 200, 200), rows)
    elif name == "waiting_confirm":
        dot(CX - 48, CY - 42, 12, dark, rows)             # 挑眉疑问
        dot(CX + 48, CY - 22, 12, dark, rows)
        dot(CX + 92, CY - 58, 7, dark, rows)              # 问号点
        for dx, dy in [(0, 0), (10, -16), (10, -30), (-6, -40), (-14, -28), (-14, -12)]:
            dot(CX + 92 + dx, CY - 40 + dy, 5, dark, rows)
    elif name == "done":
        dot(CX - 48, CY - 30, 12, dark, rows)             # 眯眼笑（横线）
        dot(CX + 48, CY - 30, 12, dark, rows)
    elif name == "error":
        # X 眼：深色笔画
        for s in (-1, 1):
            ex = CX + s * 48
            for d in range(-14, 15, 2):
                dot(ex + d, CY - 30 + d, 4, dark, rows)
                dot(ex + d, CY - 30 - d, 4, dark, rows)
    if name != "thinking":                                # 嘴（thinking 无嘴）
        smile = name != "error"
        mouth(CX, CY + 28, 60 if smile else 44, 22 if smile else 16, rows, smile)
    return rows


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "assets_png")
    os.makedirs(out, exist_ok=True)
    for name, rgb in STATES.items():
        path = os.path.join(out, f"{name}.png")
        write_png(path, gen_state(name, rgb))
        print(f"  {path} ({os.path.getsize(path)} bytes)")


if __name__ == "__main__":
    main()
