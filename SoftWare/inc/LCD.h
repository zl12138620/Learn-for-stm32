/**
  ******************************************************************************
  * @file    SoftWare/inc/LCD.h
  * @brief   1.44 寸 SPI TFT 彩屏驱动 —— ST7735S / 128x128
  *
  *          模块: LCKFB/微雪同款 1.44 inch SPI Module(MSP1443)
  *          接口: 4 线制 SPI, 8 个引脚
  *
  *          接线(信号线走硬件 SPI2, 在排针 PB[4:15] 上):
  *            VCC   -> 3.3V
  *            GND   -> 开发板 GND
  *            SCK   -> PB13   (SPI2_SCK,  AF5)
  *            SDA   -> PB15   (SPI2_MOSI, AF5)
  *            CS    -> PB12
  *            RESET -> PB11
  *            A0    -> PB10   (A0 就是 DC: 低=命令, 高=数据)
  *            LED   -> 3.3V   (背光, 手册说明可直接接电源; 想调光就改接空脚)
  *
  *          坐标: 左上角为原点, x 向右 0~127, y 向下 0~127。
  *
  *          ⚠ 128x128 不是 128x160:
  *            网上流传的 ST7735 初始化序列大多是给 128x160 屏用的, 直接拿来
  *            会花屏或只显示一部分。本驱动按手册的 128x128 写。
  *            另外这种屏的显存起点不一定在 (0,0) —— 见 LCD.c 里的
  *            LCD_COL_OFFSET / LCD_ROW_OFFSET, 显示偏移时调它。
  ******************************************************************************
  */

#ifndef __LCD_H
#define __LCD_H

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ RGB565 常用颜色 ============================ */
#define LCD_BLACK       0x0000
#define LCD_WHITE       0xFFFF
#define LCD_RED         0xF800
#define LCD_GREEN       0x07E0
#define LCD_BLUE        0x001F
#define LCD_YELLOW      0xFFE0
#define LCD_CYAN        0x07FF
#define LCD_MAGENTA     0xF81F
#define LCD_GRAY        0x8410
#define LCD_ORANGE      0xFD20

/* 屏幕尺寸。
   ⚠ 1.8 寸是 128x160, 1.44 寸才是 128x128 —— 同一家这两个模块引脚名
   一模一样(都是 LED/SCK/SDA/A0/RESET/CS/GND/VCC, 都走 ST7735S),
   极易拿错手册。拿错的表现很有迷惑性: 画面能出来, 但会有一条约占
   高度 20% 的花屏带(160/128 = 1.25, 正好剩两成没写到)且上下颠倒。 */
#define LCD_W           128U
#define LCD_H           160U

/* ============================ API ============================ */
void LCD_Init(void);                                     /* 复位 + 初始化并点亮 */

void LCD_Clear(uint16_t color);                          /* 整屏填充 */
void LCD_Fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  uint16_t color, uint8_t filled);

/* 文字: 用 OLED_Font.h 里那张 8x16 ASCII 字库(两个屏共用一张表) */
void LCD_ShowChar(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg);
void LCD_ShowString(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void LCD_ShowInt(uint16_t x, uint16_t y, int32_t v, uint16_t fg, uint16_t bg);

/* 自检: 画边框 + 三色块 + 一行字, 用来一眼判断初始化对不对 */
void LCD_SelfTest(void);

/* 排障用: 读回 SPI2 的 CR1 / SR。
   正常时 CR1 应含 SPE(0x0040, 使能) | MSTR(0x0004, 主机) |
   SSM(0x0200) | SSI(0x0100) | BR(0x0008, 4 分频) —— 即 0x034C。
   SR 的 bit8(0x0100) 是 MODF(主模式故障); 它一旦置位, MSTR 会被硬件清掉,
   SPI 变成从机, 一个字节都发不出去。 */
uint16_t LCD_SpiCR1(void);
uint16_t LCD_SpiSR(void);

/* 运行时改扫描方向 / 显存偏移(排障用)。
   madctl: 写进 0x36 的值; bit7=MY(行方向) bit6=MX(列方向) bit3=BGR。
           MY=1 -> 画面上下翻转; MX=1 -> 左右翻转。
   colOff/rowOff: 显存偏移, 画面整体偏移或被切边就调它。 */
void LCD_SetRotation(uint8_t madctl, uint8_t colOff, uint8_t rowOff);

#endif
