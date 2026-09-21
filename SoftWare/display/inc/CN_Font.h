/**
  ******************************************************************************
  * @file    SoftWare/inc/CN_Font.h
  * @brief   16x16 汉字点阵表
  *
  *          ⚠ 本文件由 工具/gen_cn_font.py 自动生成, 不要手改。
  *            要改字符集: 编辑脚本里的 CHARS, 重跑脚本, 它会重写 .h 和 .c。
  *
  *          字体: simhei.ttf (SimHei 黑体), 字号 16
  *          字符集: 主菜单摄像头舵机角度旋转选择按下进入退出帧率无信号调
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
#define CN_FONT_COUNT  26U      /* 字符集里的字数 */

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
