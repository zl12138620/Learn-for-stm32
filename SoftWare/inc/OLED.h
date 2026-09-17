#ifndef __OLED_H
#define __OLED_H
#include "stm32f4xx.h"



/* 关于 RES#/CS#/D-C# 三个控制脚 —— 本工程不占用任何单片机引脚:
     RES#  -> 3.3V   : 手册要求 "Keep this pin HIGH during normal operation",
                       靠芯片内部上电复位(POR), 不需要 GPIO 产生复位脉冲
     CS#   -> GND    : 片选低有效, 拉低才允许通信
     D/C#  -> GND    : I2C 模式下它是地址选择脚 SA0, 接 GND 得到地址 0x3C
                       接 VDD 则变成 0x3D, 与代码里的 0x78 不符
   三者都只需要固定电平, 所以原先的 RES_PIN/CS_PIN/DC_PIN 宏已删除,
   顺带解除了与 PB4(蜂鸣器)/PB5(编码器B相)/PB6(编码器A相)的冲突。 */


/* 软件 I2C 引脚初始化(PB8=SCL, PB9=SDA)。
   硬件 I2C1 跑完后也调它来把引脚切回 GPIO, 恢复软件 I2C */
void OLED_I2C_Init(void);

void OLED_Init(void);
void OLED_WriteCommand(uint8_t Command);   /* 供 main.c 的软/硬件 I2C 对比测试调用 */
void OLED_Clear(void);
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char);
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String);
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length);
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);

#endif
