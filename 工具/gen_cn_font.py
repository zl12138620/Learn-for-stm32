#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从系统字体生成 16x16 汉字点阵, 输出 SoftWare/inc/CN_Font.h + SoftWare/src/CN_Font.c

为什么要用脚本生成而不是手写字模:
    手写 16x16 点阵(每个字 32 字节)极易写错, 而且错了之后在屏幕上是"字看着
    有点怪", 很难判断是错在哪一位。用真实字体渲染, 字形一定是对的。

用法:
    python 工具/gen_cn_font.py            # 生成 .h/.c
    python 工具/gen_cn_font.py --preview  # 只打 ASCII 预览, 不写文件

点阵格式: 16 行 x 16 列, 每行 2 字节, 共 32 字节/字, 行优先, 高位在左。
    第 r 行的第 0 列 -> CN_Font16[i][r*2]   的 bit7
    第 r 行的第 8 列 -> CN_Font16[i][r*2+1] 的 bit7
"""

import sys
import os
from PIL import Image, ImageDraw, ImageFont

# Windows 控制台默认是 GBK, 直接 print 汉字会乱码。强制 UTF-8 输出。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

# ============================ 配置 ============================
FONT_PATH = "C:/Windows/Fonts/simhei.ttf"   # 黑体: 笔画粗、小字号下比宋体清楚
FONT_SIZE = 16
THRESHOLD = 128                             # 渲染灰度 > 此值算一个亮点

# 屏幕上一共要用到的字。改这里之后重跑脚本即可。
#
# ⚠ 2026-09-22 补: 原来的表里**漏了"实时画面"四个字**, 而主菜单第一项的说明
#   正好就是"实时画面" —— 结果是那四个格子画出来是**空白**的。
#   这个 bug 能潜伏下来是因为 LCD_ShowCN 对表外字的处理是"填背景色、宽度照占"
#   (见 LCD.c 的注释): 布局一点没乱, 只是那一格空的, 很容易被当成"设计如此"。
#   往 CHARS 里加字之后一定要**对着界面把每个字都看一遍**。
CHARS = "主菜单摄像头舵机角度旋转选择按下进入退出帧率无信号调实时画面"

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 2026-09-22 目录按功能领域重组后, 字模归到 display/
OUT_H = os.path.join(PROJ, "SoftWare", "display", "inc", "CN_Font.h")
OUT_C = os.path.join(PROJ, "SoftWare", "display", "src", "CN_Font.c")


def render_glyph(ch, font):
    """把一个字渲染成 16x16 的 0/1 二维列表(1 = 亮点)。

    ⚠ 按**固定原点 (0,0)** 画, 千万不要逐字裁剪墨迹再各自居中。
      汉字是字体厂商在同一个 em 框里统一设计好的, 墨迹高低本来就该不一样
      (实测 SimHei 16px: "主" 占第 1~14 行, "单" 占第 0~15 行)。
      各自居中会把这个设计意图抹掉, 于是"主"和"菜"在屏幕上错开一两行,
      一整行字看着参差不齐。
      PIL 的 text() 默认锚点是左上(ascender 线), 16px 黑体渲染出来
      墨迹正好落在 0~15 行、0~15 列内, 不会溢出。
    """
    cell = Image.new("L", (FONT_SIZE, FONT_SIZE), 0)
    ImageDraw.Draw(cell).text((0, 0), ch, font=font, fill=255)

    px = cell.load()
    return [[1 if px[x, y] > THRESHOLD else 0
             for x in range(FONT_SIZE)]
            for y in range(FONT_SIZE)]


def glyph_to_bytes(bits):
    """16x16 的 0/1 矩阵 -> 32 字节(每行 2 字节, 高位在左)。"""
    out = []
    for row in bits:
        hi = 0
        lo = 0
        for x in range(8):
            if row[x]:
                hi |= 0x80 >> x
        for x in range(8, 16):
            if row[x]:
                lo |= 0x80 >> (x - 8)
        out.append(hi)
        out.append(lo)
    return out


def preview(ch, data):
    """把点阵打成 ASCII, 用来肉眼确认字形没画歪。
    data 是 glyph_to_bytes() 的输出(32 字节), 这里再拆回 16 行。"""
    print("  %s  U+%04X" % (ch, ord(ch)))
    for r in range(16):
        hi = data[r * 2]
        lo = data[r * 2 + 1]
        line = ""
        for x in range(8):
            line += "##" if (hi >> (7 - x)) & 1 else ".."
        for x in range(8):
            line += "##" if (lo >> (7 - x)) & 1 else ".."
        print("      " + line)
    print()


def main():
    if not os.path.exists(FONT_PATH):
        sys.exit("找不到字体: %s" % FONT_PATH)

    font = ImageFont.truetype(FONT_PATH, FONT_SIZE)

    # 去重但保持顺序
    seen = set()
    chars = []
    for ch in CHARS:
        if ch not in seen:
            seen.add(ch)
            chars.append(ch)

    glyphs = [(ch, glyph_to_bytes(render_glyph(ch, font))) for ch in chars]

    if "--preview" in sys.argv:
        print("字符集 %d 个字:\n" % len(glyphs))
        for ch, bits in glyphs:
            preview(ch, bits)
        return

    n = len(glyphs)

    # ---------------- CN_Font.h ----------------
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/**
  ******************************************************************************
  * @file    SoftWare/inc/CN_Font.h
  * @brief   16x16 汉字点阵表
  *
  *          ⚠ 本文件由 工具/gen_cn_font.py 自动生成, 不要手改。
  *            要改字符集: 编辑脚本里的 CHARS, 重跑脚本, 它会重写 .h 和 .c。
  *
  *          字体: %s (SimHei 黑体), 字号 %d
  *          字符集: %s
  *
  *          点阵格式: 每字 32 字节 = 16 行 x 每行 2 字节, 行优先, 高位在左。
  *                    第 r 行左半边 -> [r*2], 右半边 -> [r*2+1]。
  ******************************************************************************
  */

#ifndef __CN_FONT_H
#define __CN_FONT_H

#include <stdint.h>

#define CN_FONT_W      16U      /* 字宽(像素) */
#define CN_FONT_H      16U      /* 字高(像素) */
#define CN_FONT_BYTES  32U      /* 每字点阵字节数 */
#define CN_FONT_COUNT  %dU      /* 字符集里的字数 */

/* UTF-8 三字节编码 -> 点阵下标的查找表。
   用法见 LCD.c 的 LCD_ShowCN(): 把 UTF-8 按 3 字节切开逐个查这张表。 */
typedef struct
{
    uint8_t utf8[3];    /* 这个字的 UTF-8 编码 */
    uint8_t index;      /* 对应 CN_Font16 的下标 */
} CN_Glyph_t;

extern const CN_Glyph_t CN_Glyphs[CN_FONT_COUNT];
extern const uint8_t    CN_Font16[CN_FONT_COUNT][CN_FONT_BYTES];

#endif /* __CN_FONT_H */
""" % (os.path.basename(FONT_PATH), FONT_SIZE, CHARS, n))

    # ---------------- CN_Font.c ----------------
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/**
  ******************************************************************************
  * @file    SoftWare/src/CN_Font.c
  * @brief   16x16 汉字点阵表 —— 由 工具/gen_cn_font.py 自动生成, 不要手改
  *
  *          字符集(%d 字): %s
  ******************************************************************************
  */

#include "CN_Font.h"

/* UTF-8 编码 -> 点阵下标 */
const CN_Glyph_t CN_Glyphs[CN_FONT_COUNT] =
{
""" % (n, CHARS))

        for i, (ch, _) in enumerate(glyphs):
            b = ch.encode("utf-8")
            f.write("    { { 0x%02XU, 0x%02XU, 0x%02XU }, %2dU },  /* %s */\n"
                    % (b[0], b[1], b[2], i, ch))

        f.write("};\n\n/* 点阵: 每字 32 字节, 16 行 x 每行 2 字节, 高位在左 */\n")
        f.write("const uint8_t CN_Font16[CN_FONT_COUNT][CN_FONT_BYTES] =\n{\n")

        for i, (ch, data) in enumerate(glyphs):
            f.write("    {   /* %s */\n" % ch)
            for r in range(16):
                f.write("        0x%02XU, 0x%02XU,%s/* 第 %2d 行 */\n"
                        % (data[r * 2], data[r * 2 + 1],
                           "  " if r < 9 else " ", r))
            f.write("    },\n")

        f.write("};\n")

    print("已生成:")
    print("  %s" % OUT_H)
    print("  %s" % OUT_C)
    print("字符集 %d 个: %s" % (n, "".join(chars)))
    print("点阵共 %d 字节" % (n * 32))


if __name__ == "__main__":
    main()
