/**
  ******************************************************************************
  * @file    SoftWare/inc/LedPwm.h
  * @brief   用户 LED(LED2 / PB2) 软件 PWM 调光
  *
  *          ⚠ 为什么是"软件"PWM:
  *            板载 LED2 硬接在 PB2 上。而 PB2 在 STM32F407 上**没有任何定时器
  *            复用通道** —— 把 TIM1~TIM5、TIM8~TIM14 的全部通道引脚穷举一遍,
  *            PB2 一次都不出现; 所以它做不了硬件 PWM, 只能靠定时器中断
  *            手动翻转引脚。硬件改不了, 这是死路。
  *
  *          定时器分配(TIM3/TIM5/TIM6/TIM7 已被占用):
  *            TIM6 -> SoftUSART 接收采样
  *            TIM7 -> Encoder  消抖窗口
  *            TIM3 -> SoftWarePWM, CH1 输出到 PB4(蜂鸣器)
  *            TIM5 -> Servo,       CH2 输出到 PA1(舵机)
  *            TIM4 -> 本模块(不用它的输出通道, 只用更新中断)
  *
  *          时基: 2MHz 计数(0.5us), 每个计数走一级亮度, 100 级一周期
  *                -> 更新中断 20kHz, 实际 LED PWM 频率 200Hz(肉眼无闪烁)
  *                -> CPU 占用约 0.3%
  *
  *          用法示例:
  *            LedPwm_Init();          // 配 PB2 + TIM4, 默认 50% 亮度
  *            LedPwm_SetDuty(0);      // 全灭
  *            LedPwm_SetDuty(100);    // 最亮
  *            LedPwm_SetDuty(35);     // 35%
  ******************************************************************************
  */

#ifndef __LEDPWM_H
#define __LEDPWM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ API ============================ */
void    LedPwm_Init(void);                     /* 配置 PB2 + TIM4, 并按默认亮度起振 */
void    LedPwm_SetDuty(uint8_t percent);       /* 亮度 0~100(%), 超出范围夹紧 */
uint8_t LedPwm_GetDuty(void);                  /* 读当前亮度 */

#ifdef __cplusplus
}
#endif

#endif /* __LEDPWM_H */
