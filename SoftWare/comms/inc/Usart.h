/**
  ******************************************************************************
  * @file    SoftWare/inc/Usart.h
  * @brief   调试串口 USART1 (PA9=TX / PA10=RX, 默认 9600)
  *
  *          本模块**自己**管着接收环形缓冲和 USART1_IRQHandler —— 中断和它
  *          读写的变量在同一个文件里, "谁改了这个变量"一眼能看出来, 不用
  *          跳到 System/stm32f4xx_it.c 去找。
  *
  *          数据流:
  *            来一个字节 -> RXNE 中断 -> 存进环形缓冲
  *            主循环调 Usart_ReadByte() 从缓冲里取
  *          这样中断里只做最少的活(取一个字节塞进缓冲), 打印/解析都在主循环。
  *
  *          ⚠ 缓冲只有 UART1_RX_BUF_SIZE 字节, 满了两头分别会丢字节 ——
  *            这是**故意**的: 调试串口丢几个字节无所谓, 总比阻塞中断强。
  *            要做可靠通信得换成带流控的方案。
  ******************************************************************************
  */

#ifndef __USART_H
#define __USART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* 接收环形缓冲的存储区大小(实际最多缓存 SIZE-1 字节, 环形缓冲的常规做法) */
#define UART1_RX_BUF_SIZE   64U

/* ============================ API ============================ */
/* 配好引脚/波特率/中断, 并使能接收。调用后就可以用下面两个函数了。 */
void    Usart_Init(uint32_t baudrate);

/* 阻塞式发送, 发完最后一个字节才返回。
   ⚠ 9600 波特率下一个字节约 1ms, 发长字符串会把主循环卡住 —— 诊断信息
     短一点, 别在实时性重要的地方连着发几十字节。 */
void    Usart_SendBytes(const uint8_t *data, uint32_t len);

/* 从接收缓冲取一个字节。取到了返回 1 并把字节写进 *ch; 缓冲空返回 0。
   非阻塞, 主循环里轮着调就行。 */
uint8_t Usart_ReadByte(uint8_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* __USART_H */
