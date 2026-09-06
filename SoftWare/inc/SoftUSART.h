/**
  ******************************************************************************
  * @file    SoftWare/inc/SoftUSART.h
  * @brief   软件串口（bit-bang UART）—— 发送 + 中断接收
  *          发送: SysTick 精确位时序, 阻塞翻转 TX(PA2)
  *          接收: RX(PA3) EXTI 下降沿检测起始位 + TIM6 定时中点采样,
  *                收完存入环形缓冲; 需 TIM6 与 EXTI 中断(在 .c 中实现强符号)
  *          用法示例:
  *              SoftUSART_Init();
  *              SoftUSART_SendString("hello\r\n");
  *              if (SoftUSART_RxReady()) ch = SoftUSART_RxByte();
  ******************************************************************************
  */

#ifndef __SOFTUSART_H
#define __SOFTUSART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ 用户配置 ============================ */
#define SOFTUSART_BAUD          9600UL        /* 波特率(1200~115200, 推荐≤115200) */

/* 发送引脚 TX */
#define SOFTUSART_TX_RCC_CLK    RCC_AHB1Periph_GPIOA
#define SOFTUSART_TX_PORT       GPIOA
#define SOFTUSART_TX_PIN        GPIO_Pin_2

/* 接收引脚 RX */
#define SOFTUSART_RX_RCC_CLK    RCC_AHB1Periph_GPIOA
#define SOFTUSART_RX_PORT       GPIOA
#define SOFTUSART_RX_PIN        GPIO_Pin_3

/* RX 中断通道: 若改动 RX 引脚, 下面 4 个宏必须同步改为对应值!
   (例: 改成 PA5 -> EXTI_PinSource5/EXTI_Line5/EXTI5_IRQn + EXTI5_IRQHandler) */
#define SOFTUSART_RX_PORT_SRC   EXTI_PortSourceGPIOA
#define SOFTUSART_RX_PIN_SRC    EXTI_PinSource3
#define SOFTUSART_RX_EXTI_LINE  EXTI_Line3
#define SOFTUSART_RX_EXTI_IRQn  EXTI3_IRQn

/* 接收环形缓冲大小 */
#define SOFTUSART_RX_BUFSZ      16

/* ============================ API ============================ */
void    SoftUSART_Init(void);            /* 初始化 TX/RX/EXTI/TIM6/中断 */
void    SoftUSART_SendByte(uint8_t ch);  /* 阻塞发送 1 字节(LSB first, 8N1) */
void    SoftUSART_SendString(const char *s);
uint8_t SoftUSART_RxReady(void);         /* 环形缓冲中可读字节数(0=空) */
uint8_t SoftUSART_RxByte(void);          /* 取 1 字节(空则返回 0) */

/* 中断服务函数(强符号, 覆盖启动文件弱符号; 注意别在 stm32f4xx_it.c 重复定义) */
void EXTI3_IRQHandler(void);
void TIM6_DAC_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __SOFTUSART_H */
