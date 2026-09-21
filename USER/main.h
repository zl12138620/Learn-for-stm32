/**
  ******************************************************************************
  * @file    USER/main.h
  * @note    本副本源自 ST 官方模板:
  *          Project/STM32F4xx_StdPeriph_Templates/main.h (V1.8.1, 27-January-2022)
  * @brief   Header for USER/main.c module
  *
  *          这里只 include **main.c 自己要用到的东西** —— 初始化清单里列的
  *          那几个模块。别的模块之间怎么依赖, 由它们各自的头文件负责,
  *          不要什么都往这里塞(以前这里塞了 8 个头文件, 拆完只剩这些)。
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
#include <string.h>

/* 与 main.c 里那张初始化清单一一对应 */
#include "Led.h"        /* system/   心跳灯 */
#include "Usart.h"      /* comms/    调试串口 */
#include "LCD.h"        /* display/  TFT 屏 */
#include "OV7670.h"     /* camera/   摄像头 */
#include "Servo.h"      /* motion/   舵机 */
#include "Encoder.h"    /* motion/   旋转编码器 */
#include "Tick.h"       /* system/   毫秒时基 */
#include "App.h"        /* app/      界面调度 */

#endif /* __MAIN_H */
