/**
  ******************************************************************************
  * @file    SoftWare/inc/Encoder.h
  * @brief   旋转编码器(增量式两相, 如 EC11)中断驱动 —— 计次 + 判向
  *
  *          接线: A 相 -> PB6, B 相 -> PB5, SW -> PB7 (本文件宏可改)
  *                内部上拉输入, 空闲/停止时 A、B 均为高电平;
  *                转动时引脚被拉到低(需与 GND 侧开关相连)。
  *
  *          ⚠ SW 原来是 PB0, 2026-09-21 改到 PB7 —— 因为 PB0 被 OV7670 的
  *            SCCB 数据线(SIO_D, 开漏)占了。按钮按下时会把 PB0 短到 GND,
  *            开机时按住按钮会让 SCCB 写出的 '1' 被拉成 '0', 寄存器配错,
  *            现象是摄像头自检报 CAM FAIL。PB7 空闲, 且与 PB5/PB6 同在
  *            GPIOB(排针 P2-22/23/24 连成一片), 接线方便。
  *
  *          中断方案(本工程最终采用):
  *            - A 相(PB6) 配置 EXTI 下降沿中断; A 一由高变低立刻进中断,
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
/* ⚠ 改 A 相引脚时, 下面四个 EXTI 宏必须跟着一起改, 否则中断会盯到别的引脚上!
   2026-09-17 踩过的坑: A/B 相从 PB8/PB7 挪到 PB6/PB5 时只改了 PIN 宏,
   EXTI 仍留在线 8, 结果 EXTI8 映射的是 PB8(OLED 的 SCL), 编码器完全失效。 */
#define ENC_GPIO_CLK        RCC_AHB1Periph_GPIOB   /* GPIOB 时钟 */
#define ENC_GPIO_PORT       GPIOB                  /* 编码器挂在 GPIOB */
#define ENC_A_PIN           GPIO_Pin_6             /* A 相 = PB6 */
#define ENC_B_PIN           GPIO_Pin_5             /* B 相 = PB5 */

/* SW 按键(编码器轴往下按): 上拉输入, 松开=高, 按下=低。
   采样在 1ms 中断里做(Tick.c 的 SysTick_Handler 会调 Encoder_SwTick1ms),
   主循环只需调 Encoder_SwTakePress() 取"按下"事件 —— 不用自己轮询电平。
   为什么必须这样: 摄像头界面一轮约 95ms, 主循环轮询会漏掉快速点按;
   而如果判电平, 一次按下会在随后每一轮都被判定为"按下", 导致进功能界面
   后立刻又弹回主菜单。 */
#define ENC_SW_PIN          GPIO_Pin_7             /* SW = PB7 */

/* EXTI 相关: A 相接 PB6 -> 用线 6; 线 5~9 共用 EXTI9_5 中断 */
#define ENC_EXTI_PORT_SRC   EXTI_PortSourceGPIOB
#define ENC_EXTI_PIN_SRC    EXTI_PinSource6
#define ENC_EXTI_LINE       EXTI_Line6
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

/* SW 按键消抖窗口。1ms 中断里每毫秒采一次, 要连续这么多次读到同一电平才认账。
   20 次 = 20ms, 足够滤掉机械抖动, 手按又不会觉得迟滞。 */
#define ENC_SW_DEBOUNCE_MS  20U

/* ============================ API ============================ */
void     Encoder_Init(void);        /* 配置 PB6/PB5/PB7 输入 + EXTI6 中断 + TIM7 + NVIC */

/* ---------- 旋转: 用这个, 不要用下面的 GetCW/GetCCW ---------- */
/* 返回"上次调用以来净转了几格"(正转正数 / 反转负数), 读的同时清零。
   为什么要有它: 切换界面时必须把残留计数冲掉, 否则上一屏攒下的格数会在
   新界面立刻生效 —— 典型现象是在主菜单转到"舵机"项按下进入, 舵机自己就
   转过去了。另外它内部关中断保护, 读数+清零是原子的, 不会在中间被
   EXTI 插进来导致多算/少算一格。 */
int32_t  Encoder_ReadDelta(void);

/* ---------- SW 按键 ---------- */
/* 主循环取一次按下事件, 取走即清; 无事件返回 0。
   (采样和消抖由 Encoder_Init() 注册给 Tick 的 1ms 回调去做, 应用层不用管) */
uint8_t  Encoder_SwTakePress(void);

/* 读 SW 的**当前电平**(已经过 20ms 消抖): 1 = 正按着, 0 = 松着。
   ⚠ 和上面那个是**两种东西**, 别拿错:
        Encoder_SwTakePress() 是**边沿事件** —— 按一下只出现一次, 取走即清,
                               适合"按一下切一个界面"这种一次性动作。
        Encoder_SwIsDown()    是**电平**     —— 按住期间一直为 1,
                               适合"按住不放要持续生效"的场合。
     拿错的后果: 用 IsDown 判单击会变成按住就狂触发; 用 TakePress 判长按
     则永远等不到第二次。
   谁在用: LVGL 的 encoder 设备要的是电平(边沿由 LVGL 自己判), 所以给它这个;
     而本工程原来那套菜单状态机用的是边沿, 保持 TakePress 不变。 */
uint8_t  Encoder_SwIsDown(void);

/* ---------- 底层计数(调试用) ---------- */
uint32_t Encoder_GetCW(void);       /* 读正转累计次数 */
uint32_t Encoder_GetCCW(void);      /* 读反转累计次数 */
void     Encoder_ResetCount(void);  /* 清零正/反转计数 */

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_H */
