/**
  ******************************************************************************
  * @file    SoftWare/inc/Servo.h
  * @brief   SG90 舵机驱动 —— PA1 / TIM5_CH2
  *
  *          定时器分配(TIM3/TIM6/TIM7 已被占用):
  *            TIM6 -> SoftUSART 接收采样
  *            TIM7 -> Encoder  消抖窗口
  *            TIM3 -> SoftWarePWM, CH1 输出到 PB4(蜂鸣器)
  *            TIM5 -> 本模块,      CH2 输出到 PA1
  *
  *          舵机时基: 50Hz(周期 20ms), 脉宽 0.5~2.5ms 对应 0~180°,
  *                    1.5ms 是中位 90°。
  *
  *          用法示例:
  *            Servo_Init();            // 配置 PA1 + TIM5_CH2, 上电回中位 90°
  *            Servo_SetAngle(0);       // 转到 0°
  *            Servo_SetAngle(180);     // 转到 180°
  *            Servo_SetPulseUs(1500);  // 直接给脉宽(us), 校准用
  *            Servo_Stop();            // 停输出, 舵机失去保持力
  *
  *          注意:
  *            - 舵机电源必须外接 5V, 不要从板子 3.3V 取电。SG90 转动
  *              100~200mA, 堵转可达 ~700mA, 会把板载 LDO 拉崩导致 MCU 复位。
  *            - 舵机的地必须和开发板共地, 否则 PWM 信号没有参考电平。
  *            - 部分 SG90 实际行程不是 500~2500us。若转到两端时听到嗡嗡声
  *              或看到抖动, 说明超出了它的机械行程: 用 Servo_SetPulseUs()
  *              试出真实端点, 再把 Servo.c 里的 SERVO_PULSE_MIN/MAX_US 改窄。
  ******************************************************************************
  */

#ifndef __SERVO_H
#define __SERVO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ API ============================ */
void     Servo_Init(void);                /* 配置 PA1 + TIM5_CH2, 50Hz, 并回中位 */
void     Servo_Stop(void);                /* 停止输出并把引脚拉低(舵机失去保持力) */

void     Servo_SetAngle(uint8_t deg);     /* 设置角度 0~180°, 超出范围自动夹紧 */
uint8_t  Servo_GetAngle(void);            /* 读当前角度 */

void     Servo_SetPulseUs(uint16_t us);   /* 直接设置脉宽(us), 校准/调试用 */
uint16_t Servo_GetPulseUs(void);          /* 读当前脉宽 */

#ifdef __cplusplus
}
#endif

#endif /* __SERVO_H */
