/**
  ******************************************************************************
  * @file    SoftWare/inc/SoftWarePWM.h
  * @brief   硬件 PWM 输出 —— PB4(板载蜂鸣器 FMQ) / TIM3_CH1
  *
  *          定时器分配(TIM6/TIM7 已被占用):
  *            TIM6 -> SoftUSART 接收采样
  *            TIM7 -> Encoder  消抖窗口
  *            TIM3 -> 本模块, CH1 输出到 PB4
  *
  *          用法示例:
  *            PWM_Init();             // 配置并使能, 默认 1kHz / 50% 占空比
  *            PWM_SetFreq(2000);      // 改成 2kHz
  *            PWM_SetDuty(80);        // 占空比改成 80%
  *            PWM_Stop();             // 静音(引脚被拉低)
  *            PWM_Start();            // 恢复刚才的 频率/占空比
  *
  *          注意:
  *            - PWM_Init() 会直接让蜂鸣器以 1kHz 响起来(和 OLED_Init 立刻显示
  *              一样, 方便一眼确认硬件通了)。不想上电就响, 紧跟着调 PWM_Stop()。
  *            - PB4 复位后是 JTAG 的 NJTRST。STM32F4 没有 F1 的 AFIO/SWJ_CFG,
  *              手册 33.4.4 节说明: 把 PB4 配成复用/普通 IO 只需设置 GPIO_MODER,
  *              不需要额外的"关 JTAG"寄存器。但若把调试方式从 SWD 换成 JTAG,
  *              该引脚会被调试口占用, 蜂鸣器就没法用了。
  *            - 计时基频 1MHz, 频率范围约 16Hz ~ 1MHz, 覆盖蜂鸣器全音域。
  ******************************************************************************
  */

#ifndef __SOFTWAREPWM_H
#define __SOFTWAREPWM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ API ============================ */
void     PWM_Init(void);                  /* 配置 PB4 + TIM3_CH1, 并按默认值使能输出 */
void     PWM_Start(void);                 /* 按当前 频率/占空比 恢复输出 */
void     PWM_Stop(void);                  /* 停止输出并把引脚拉低(静音) */

void     PWM_SetFreq(uint32_t hz);        /* 设置频率(Hz), 约 16 ~ 1000000 */
void     PWM_SetDuty(uint8_t percent);    /* 设置占空比 0~100(%) */

uint32_t PWM_GetFreq(void);               /* 读当前频率 */
uint8_t  PWM_GetDuty(void);               /* 读当前占空比 */

#ifdef __cplusplus
}
#endif

#endif /* __SOFTWAREPWM_H */
