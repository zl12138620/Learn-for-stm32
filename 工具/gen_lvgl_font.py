#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 LVGL 用的字库, 输出到 SoftWare/ui/src/。

    python 工具/gen_lvgl_font.py

目前生成两个:
    lv_font_simhei_16.c     16px, 界面上要用的全部汉字 + 可打印 ASCII
    lv_font_simhei_28_num.c 28px, **只有数字和空格** —— 舵机那个大角度值
                            (设计上它是那一屏唯一的主角, 所以单独放大;
                             不需要汉字, 所以只有 11 个字形, 几 KB flash)

和 工具/gen_cn_font.py 是**三件不同的事**, 别搞混:
    gen_cn_font.py       -> display 领域那套**手写驱动**用的 16x16 位图(CN_Font.c)
    gen_lvgl_font.py     -> LVGL 用的字体(本文件)
    (设计稿) docs/UI设计稿-v1.py -> 用 PIL 画的**效果图**, 不是真字库

界面上要用到的字变了, **两个脚本都要改并重跑**(手写那套还在用)。

================================================================================
为什么要包一层脚本, 而不是直接跑 npx lv_font_conv
================================================================================

因为`lv_font_conv`生成出来的文件**缺两项关键字段**, 必须手工补。而生成的文件
开头写着"自动生成", 谁也不会想到里面有手工内容 —— 下次重新生成就会**静默丢掉**,
现象是"字能显示, 但每画一个字就 malloc 一次", 池子碎片化, 极难查。

所以补丁写在这里, 每次生成完自动打上, 并且校验。

缺的两项(2026-09-22 实测, 用 npx 拉到的版本):

  1. .static_bitmap = 1
     少了它的后果是**性能**, 不是功能: LVGL 画字的零拷贝快路径要求
     "static_bitmap 且字形格式是 A8" 同时成立
     (见 LVGL/src/draw/sw/lv_draw_sw_letter.c:142-144)。
     不成立就要为每个字模 malloc 一块 A8 缓冲再展开, 每画一次标签
     malloc/free 一轮 —— 池子会被磨出碎片。

  2. .cap_height / .x_height
     只影响混排文字的垂直对齐。缺了不会崩, 但中文和 ASCII 的基线会对不齐。

================================================================================
为什么 --bpp 必须是 8
================================================================================

不是大小问题, 是上面那条快路径的问题: 它要求字形格式是 LV_FONT_GLYPH_FORMAT_A8。
用 --bpp 1/2/4 的话每个字模都要走慢路径(先 malloc, 再把低位深展开成 8bpp)。

代价: 16px 那套约 100KB 源码 / 14KB 实际字形数据; 28px 数字那套只有 11 个字形。
F407VGT6 有 1MB flash, 完全不值得为省这点空间换回慢路径。
"""

import os
import re
import subprocess
import sys

# Windows 控制台默认 GBK, 直接 print 汉字会乱码
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

# ============================ 配置 ============================

FONT_PATH = "C:/Windows/Fonts/simhei.ttf"

# 界面上要用到的全部汉字。
# ⚠ 和 工具/gen_cn_font.py 的 CHARS 保持一致, 改一个就要改另一个。
#   漏字的现象是屏幕上出现**空白方块**(LVGL 的 placeholder), 不会报错。
CHARS_UI = "主菜单摄像头舵机角度旋转选择按下进入退出帧率无信号调实时画面"

# 28px 那套只要数字和空格。⚠ 别往里加汉字 —— 那会让这个文件白白变大,
# 而且 28px 的汉字在 128 宽的屏上一行放不下几个。
CHARS_NUM = " 0123456789"

FONTS = [
    {"name": "lv_font_simhei_16",     "size": 16, "chars": CHARS_UI,  "ascii": True},
    {"name": "lv_font_simhei_28_num", "size": 28, "chars": CHARS_NUM, "ascii": False},
]

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(PROJ, "SoftWare", "ui", "src")


def build_range_arg(chars, with_ascii):
    """把字符集转成 lv_font_conv 的 --range 参数。

    ⚠ 这里刻意**不**用 --symbols 直接传中文: 命令行的编码要经过 Git Bash ->
      Windows CreateProcess -> node, 任何一环按 GBK 处理都会把汉字弄坏,
      而且失败时是"少几个字形"这种不明显的症状。
      转成十六进制码点是 ASCII, 一路无歧义, 而且生成的命令行可核对、可复现。
    """
    codes = sorted(set(ord(c) for c in chars))
    parts = ["0x%04X" % c for c in codes]
    if with_ascii:
        parts.append("0x20-0x7F")      # 可打印 ASCII(界面里有 "0" "180" 这种)
    return ",".join(parts)


def measure_metrics(size):
    """从字体里**量**出 cap_height / x_height, 不靠猜。

    做法就是渲染一个 'H' 和一个 'x', 看墨迹占几行像素。
    (定 LV_MEM_SIZE 时凭感觉估过一次, 结果白占 24KB RAM —— 这次直接量。)
    """
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("!! 没装 Pillow, 无法量 cap_height/x_height, 用回退值")
        return (size * 11 // 16), (size * 8 // 16)

    f = ImageFont.truetype(FONT_PATH, size)

    def ink_height(ch):
        img = Image.new("L", (size * 2, size * 2), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=f, fill=255)
        b = img.getbbox()
        return (b[3] - b[1]) if b else 0

    return ink_height("H"), ink_height("x")


def gen_one(spec):
    name = spec["name"]
    size = spec["size"]
    out_c = os.path.join(OUT_DIR, name + ".c")

    cap_h, x_h = measure_metrics(size)
    print("  %s: %dpx  cap=%d x=%d  字形 %d 个" %
          (name, size, cap_h, x_h, len(set(spec["chars"]))))

    cmd = [
        "npx", "--yes", "lv_font_conv",
        "--font", FONT_PATH,
        "--size", str(size),
        "--bpp", "8",                  # ⚠ 必须 8, 见文件头
        "--no-compress",
        "--format", "lvgl",
        "--range", build_range_arg(spec["chars"], spec["ascii"]),
        "--lv-font-name", name,
        "-o", out_c,
    ]

    r = subprocess.run(cmd, cwd=PROJ, shell=(os.name == "nt"))
    if r.returncode != 0:
        print("  !! lv_font_conv 失败, 退出码 %d" % r.returncode)
        return False

    # ---------------- 打补丁 ----------------
    with open(out_c, "r", encoding="utf-8") as fp:
        src = fp.read()
    original = src

    # 补 1: cap_height / x_height, 插在 base_line 后面(和 montserrat 的排法一致)
    if ".cap_height" not in src:
        src, n = re.subn(
            r"(\.base_line = -?\d+,\s*/\*[^\n]*\*/\n)",
            r"\1"
            "#if LV_VERSION_CHECK(9, 6, 0) || LVGL_VERSION_MAJOR >= 10\n"
            "    .cap_height = %d,           /*by 工具/gen_lvgl_font.py: 从字体实测*/\n"
            "    .x_height = %d,             /*by 工具/gen_lvgl_font.py: 从字体实测*/\n"
            "#endif\n" % (cap_h, x_h),
            src, count=1)
        if not n:
            print("  !! 没匹配到 base_line, cap/x_height 没补上")

    # 补 2: static_bitmap, 插在 underline_thickness 后面
    if ".static_bitmap" not in src:
        src, n = re.subn(
            r"(\.underline_thickness = \d+,\n)",
            r"\1"
            "#if LV_VERSION_CHECK(9, 3, 0)\n"
            "    .static_bitmap = 1,    /*by 工具/gen_lvgl_font.py: 缺它零拷贝快路径不成立*/\n"
            "#endif\n",
            src, count=1)
        if not n:
            print("  !! 没匹配到 underline_thickness, static_bitmap 没补上")

    # 在文件头加一段说明, 免得下一个人以为这文件纯自动生成
    banner = (
        "/*\n"
        " * ⚠ 本文件由 `工具/gen_lvgl_font.py` 生成并**自动打了补丁**, 不要手改。\n"
        " *   直接跑 npx lv_font_conv 得到的文件缺 .static_bitmap / .cap_height /\n"
        " *   .x_height 三项, 后果见那个脚本的说明。要重新生成就跑那个脚本。\n"
        " */\n"
    )
    if "gen_lvgl_font.py" not in src:
        idx = src.find("*****")
        src = src[:idx] + banner + src[idx:] if idx > 0 else banner + src

    if src != original:
        with open(out_c, "w", encoding="utf-8") as fp:
            fp.write(src)

    # ---------------- 校验 ----------------
    ok = True
    with open(out_c, "r", encoding="utf-8") as fp:
        final = fp.read()
    for needle in (".static_bitmap = 1", ".cap_height =", ".x_height ="):
        if needle not in final:
            print("  !! 校验失败, 缺 %s" % needle)
            ok = False
    print("  -> %s (%d 字节)  %s" % (name + ".c", len(final), "OK" if ok else "有问题"))
    return ok


def main():
    if not os.path.isfile(FONT_PATH):
        print("找不到字体: %s" % FONT_PATH)
        return 1

    print("生成 LVGL 字库(首次会联网下载 lv_font_conv)...")
    all_ok = True
    for spec in FONTS:
        if not gen_one(spec):
            all_ok = False

    print("结果: %s" % ("全部 OK" if all_ok else "有失败, 见上面"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
