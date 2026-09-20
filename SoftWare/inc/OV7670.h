/**
  ******************************************************************************
  * @file    SoftWare/inc/OV7670.h
  * @brief   OV7670 摄像头(带 AL422B FIFO 的模块) —— 抓帧 + 读出
  *
  *          接线(模块 P1 是 2x11 = 22 脚):
  *            1  VCC       -> 3.3V
  *            2  GND       -> GND
  *            3  SIO_C     -> PE15   SCCB 时钟
  *            4  SIO_D     -> PB0    SCCB 数据(开漏)
  *            5  VSYNC     -> PB1    帧同步(输入)
  *            6  HREF      -> 不接
  *            7  D7        -> PE7  ┐
  *            8  D6        -> PE6  │
  *            9  D5        -> PE5  │
  *           10  D4        -> PE4  ├ 数据线全挂在 PE 低 8 位,
  *           11  D3        -> PE3  │ 一次 GPIOE->IDR 就能读回一整个字节
  *           12  D2        -> PE2  │
  *           13  D1        -> PE1  │
  *           14  D0        -> PE0  ┘
  *           15  RESET     -> PE13
  *           16  PWDN      -> PE14   (嫌线多可以直接接 GND)
  *           17  STROBE    -> 不接
  *           18  FIFO_RCK  -> PE8    读时钟
  *           19  FIFO_WR   -> PE12   捕获使能
  *           20  FIFO_OE   -> PE9    输出使能(低有效)
  *           21  FIFO_WRST -> PE11   写指针复位(低有效)
  *           22  FIFO_RRST -> PE10   读指针复位(低有效)
  *
  *          为什么带 FIFO 的版本好接:
  *            摄像头自己的 PCLK 直接接去驱动 FIFO 的写时钟, 模块上还有 12MHz
  *            晶振产生 XCLK —— **MCU 从头到尾不接触像素时钟**, 只按自己的节奏
  *            从 FIFO 里读。低速 MCU 能接 OV7670, 靠的就是这一点。
  *
  *          模块内部: FIFO_WE = NAND(FIFO_WR, HREF), 所以 FIFO_WR 是 MCU
  *          控制的"捕获使能", 而且只在 HREF 有效期间才真正写入 ——
  *          抓到的正好是有像素的区域, 不用管消隐期。
  *
  *          分辨率路线: 摄像头出 **QVGA 320x240**(配置最成熟可靠),
  *          读出时**隔行隔列抽点 2:1** 得到 160x120, 再旋转 90° 成 120x160
  *          铺满 TFT。用 QQVGA 少读一半数据, 但 QQVGA 的寄存器配置在公开
  *          资料里不如 QVGA 可靠, 权衡后走了 QVGA 这条路。
  ******************************************************************************
  */

#ifndef __OV7670_H
#define __OV7670_H

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ 尺寸 ============================ */
#define CAM_W       320U    /* 摄像头输出 QVGA 宽度 */
#define CAM_H       240U
#define CAM_BYTES   (CAM_W * CAM_H * 2U)      /* 153600 字节, FIFO 有 393216 */

#define CAM_OUT_W   160U    /* 抽点 2:1 之后 */
#define CAM_OUT_H   120U

#define CAM_ROT_W   120U    /* 再转 90°, 正好铺满 128x160 的屏 */
#define CAM_ROT_H   160U

/* ============================ API ============================ */
void      OV7670_Init(void);                /* GPIO + 复位 + SCCB 写寄存器表 */

/* 读回一个寄存器。这是**接好线之后第一个该调的函数**:
   读 PID(0x0A)/VER(0x0B), 正常应回 0x76 / 0x73。
   读得到 -> SCCB 通了(接线、上拉、供电都没问题); 读不到 -> 先查 SIO_C/SIO_D。 */
uint8_t   OV7670_ReadReg(uint8_t reg);
void      OV7670_WriteReg(uint8_t reg, uint8_t val);

void      OV7670_CaptureFrame(void);        /* 对齐帧边界 -> 抓一帧进 FIFO */
void      OV7670_ReadFrameRotated(void);    /* 读出 + 抽点 + 旋转, 存进帧缓冲 */
uint16_t *OV7670_GetFrameBuf(void);         /* 帧缓冲, 尺寸 CAM_ROT_W x CAM_ROT_H */

#endif
