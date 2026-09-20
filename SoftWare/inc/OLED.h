/**
  ******************************************************************************
  * @file    SoftWare/inc/OLED.h
  * @brief   1.3 寸 OLED(SSD1315 驱动, 128x64) 软件 I2C 驱动
  *
  *          接线(4 脚模块):
  *            GND -> 开发板 GND
  *            VCC -> 3.3V          (SSD1315 模块板上带升压, 别接 5V)
  *            SCL -> PB8
  *            SDA -> PB9
  *
  *          为什么用 SSD1306 的命令序列驱动 SSD1315:
  *            SSD1315 的命令集与 SSD1306 兼容, 显存也是 128x64 无偏移,
  *            所以同一套初始化直接可用。(注意 1.3 寸里还有一种是 SH1106,
  *            显存 132 列且可见区从第 2 列开始, 那种必须做列偏移 +2,
  *            两者接错会花屏。本驱动按 SSD1315/SSD1306 写。)
  *
  *          坐标约定:
  *            Line   = 行, 1~4   (每行 16 像素高的字符)
  *            Column = 列, 1~16  (每列 8 像素宽)
  ******************************************************************************
  */

#ifndef __OLED_H
#define __OLED_H

#include "stm32f4xx.h"

/* 把 PB8/PB9 配成开漏输出(总线空闲)。
   OLED_Init() 内部会调它; 想在初始化之前单独体检总线时可以先调这个。 */
void OLED_I2C_Init(void);

/* 设置 I2C 半周期延时(空转循环次数), 值越大越慢。
   默认 40 约合 350kHz, 对应"模块自带 4.7k 上拉"的正常情况。
   模块若没焊上拉电阻, 只剩 STM32 内部约 40k 弱上拉, 必须调大到 400 以上
   (约 35kHz)才能让从机看到有效时钟 —— 否则表现为一直 NO ACK。 */
void OLED_SetSpeed(uint32_t loops);

/* 总线体检: 把两根线都放开, 读回真实电平。
   返回位图: bit0 = SCL 为低, bit1 = SDA 为低。返回 0 = 总线空闲(两根都高)。
   注意: 必须先调过 OLED_I2C_Init()。 */
uint8_t OLED_BusCheck(void);

/* 总线卡死恢复。从机若卡在一位中间, 会把 SDA 一直拉住不放(I2C 经典故障);
   这里手动补最多 9 个 SCL 时钟让它把这一位走完, 再补一个 STOP 复位从机的
   状态机。返回恢复之后的总线状态(含义同 OLED_BusCheck)。 */
uint8_t OLED_BusRecover(void);

/* 探测屏是否应答(发从机地址看有没有 ACK)。
   返回 1 = 有应答(接线/上拉/供电正常); 0 = 无应答。
   屏幕全黑时先调它, 能把"驱动写错了"和"线没接好"分开。 */
uint8_t OLED_Probe(void);

void OLED_Init(void);
void OLED_Clear(void);
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char);
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String);
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length);
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);

#endif
