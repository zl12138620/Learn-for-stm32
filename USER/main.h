/**
  ******************************************************************************
  * @file    USER/main.h
  * @note    本副本源自 ST 官方模板:
  *          Project/STM32F4xx_StdPeriph_Templates/main.h (V1.8.1, 27-January-2022)
  * @brief   Header for USER/main.c module
  *          （若上游模板更新，请同步此文件）
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx.h"
#include <stdbool.h>
#include "Ring_buffer.h"

/* ============ USART1 接收环形缓冲(中断接收 -> 主循环读取) ============ */
#define UART1_RX_BUF_SIZE  64   /* 存储区 64 字节; 环形缓冲实际最多缓存 63 字节 */

/* 实例定义在 USER/main.c; 供 USART1_IRQHandler(stm32f4xx_it.c) 与 main 共用 */
extern RingBuf_t   g_uart1_rx;
extern uint8_t     g_uart1_rx_mem[UART1_RX_BUF_SIZE];

#endif /* __MAIN_H */
