/**
  ******************************************************************************
  * @file    SoftWare/src/Tick.c
  * @brief   1ms 毫秒时基(SysTick)实现 —— 详见 Tick.h 的说明
  ******************************************************************************
  */

#include "Tick.h"

/* 1ms 中断里累加; 中断写、主循环读, 所以是 volatile。
   32 位读写是单条指令, 不需要临界区保护。 */
static volatile uint32_t s_tick_ms = 0U;

/* 可选的 1ms 回调, 由 Tick_SetMsCallback() 注册。
   注册/读取都是单条 32 位存取, 不需要临界区。 */
static void (*s_ms_cb)(void) = 0;

void Tick_Init(void)
{
  /* SystemCoreClock = 168000000, /1000 = 168000 < 2^24(16777216), 装得下。
     SysTick_Config() 顺带把 SysTick 中断优先级设成最低, 正合我意 ——
     它只做计时, 不该去抢编码器和串口的中断。 */
  (void)SysTick_Config(SystemCoreClock / 1000U);
}

uint32_t Tick_GetMs(void)
{
  return s_tick_ms;
}

void Tick_SetMsCallback(void (*fn)(void))
{
  s_ms_cb = fn;
}

uint8_t Tick_Elapsed(uint32_t start, uint32_t ms)
{
  /* 无符号减法: s_tick_ms 回绕时差值依然正确, 不用特判 */
  return ((uint32_t)(s_tick_ms - start) >= ms) ? 1U : 0U;
}

/* ========================================================================
 * SysTick 中断(每 1ms 一次)
 *
 * ⚠ 这个符号原本在 System/stm32f4xx_it.c 里有个空壳, 已经删掉了 ——
 *   同一个中断向量定义两处会链接报 "multiple definition of `SysTick_Handler'"。
 *   本工程惯例是中断处理函数放在各自的驱动文件里(EXTI9_5/TIM7 在 Encoder.c),
 *   SysTick 归时基模块, 就放这儿。
 * ====================================================================== */
void SysTick_Handler(void)
{
  s_tick_ms++;

  /* 有谁注册了 1ms 回调就调它(目前是编码器的按键采样)。
     Tick 本身**不认识**任何外设 —— 谁需要谁来注册, 这样 system/ 不依赖
     任何上层领域, 可以整块搬走。 */
  if (s_ms_cb != 0)
  {
    s_ms_cb();
  }
}
