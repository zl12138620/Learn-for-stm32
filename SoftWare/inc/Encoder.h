/**
  ******************************************************************************
  * @file    SoftWare/inc/Encoder.h
  * @brief   旋转编码器(增量式两相, 如 EC11)中断驱动 —— 计次 + 判向
  *
  *          接线: A 相 -> PB8, B 相 -> PB7 (本文件宏可改)
  *                内部上拉输入, 空闲/停止时 A、B 均为高电平;
  *                转动时引脚被拉到低(需与 GND 侧开关相连)。
  *
  *          中断方案(本工程最终采用):
  *            - A 相(PB8) 配置 EXTI 下降沿中断; A 一由高变低立刻进中断,
  *              此刻读 B 相: B=低 -> 反转(CCW); B=高 -> 正转(CW)
  *              (边沿瞬间采样, 比轮询更准, 主循环忙也不丢转)
  *            - 计数后立即屏蔽 EXTI 并启动 TIM7 定时 3ms(消抖窗口);
  *              TIM7 中断到期后再放开 EXTI -> 抖动/其它沿全部被忽略,
  *              保证“转一格并松手”恰好计一次
  *
  *          用法示例:
  *            Encoder_Init();                 // GPIO+EXTI+TIM7+NVIC 一次性配置
  *            // 不需要任何轮询! 计数由 EXTI9_5 / TIM7 中断自动完成
  *            cw  = Encoder_GetCW();          // 随时读正转次数
  *            ccw = Encoder_GetCCW();         // 随时读反转次数
  ******************************************************************************
  */

#ifndef __ENCODER_H
#define __ENCODER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx.h"

/* ============================ 引脚/参数配置 ============================ */
#define ENC_GPIO_CLK        RCC_AHB1Periph_GPIOB   /* GPIOB 时钟 */
#define ENC_GPIO_PORT       GPIOB                  /* 编码器挂在 GPIOB */
#define ENC_A_PIN           GPIO_Pin_8             /* A 相 = PB8 */
#define ENC_B_PIN           GPIO_Pin_7             /* B 相 = PB7 */

/* EXTI 相关: A 相接 PB8 -> 线 8 属于 EXTI9_5 组 */
#define ENC_EXTI_PORT_SRC   EXTI_PortSourceGPIOB
#define ENC_EXTI_PIN_SRC    EXTI_PinSource8
#define ENC_EXTI_LINE       EXTI_Line8
#define ENC_EXTI_IRQn       EXTI9_5_IRQn

/* 去抖定时器: 用 TIM7 做 3ms 消抖窗口(计数后再放开 EXTI)。
   注意: 宏按 APB1 定时器时钟 84MHz 计算(168MHz 主频, APB1=42MHz, 定时器 x2)。
   若改系统时钟, 请同步调整 ENC_TIM_PSC/ENC_TIM_ARR:
     ENC_TIM_PSC = 定时器时钟/1MHz - 1   (83 -> 1MHz 计数)
     ENC_TIM_ARR = 去抖毫秒数 x 1000 - 1  (2999 -> 3ms)  */
#define ENC_TIM_CLK         RCC_APB1Periph_TIM7
#define ENC_TIM_PSC         83U
#define ENC_TIM_ARR         2999U
#define ENC_DEBOUNCE_MS     3U   /* 消抖窗口毫秒数(说明用, 由上面两个宏实现) */

/* ============================ API ============================ */
void     Encoder_Init(void);        /* 配置 PB7/PB8 输入 + EXTI8 中断 + TIM7 + NVIC */
uint32_t Encoder_GetCW(void);       /* 读正转累计次数 */
uint32_t Encoder_GetCCW(void);      /* 读反转累计次数 */
void     Encoder_ResetCount(void);  /* 清零正/反转计数 */

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_H */
