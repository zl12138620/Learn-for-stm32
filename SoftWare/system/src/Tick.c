/**
  ******************************************************************************
  * @file    SoftWare/system/src/Tick.c
  * @brief   毫秒时基 —— FreeRTOS 接管 SysTick 之后的包装层
  *
  *          2026-09-22 移植 FreeRTOS 前, 这个文件自己用 SysTick 做 1ms 时基
  *          (自己写 SysTick_Handler、自己 SysTick_Config)。现在 SysTick 归
  *          内核了 —— 它的心跳就是 FreeRTOS 的 tick, 所以这里只剩一层包装。
  *
  *          这么包一层而不是让上层直接调 xTaskGetTickCount() 的理由:
  *            1. Led.c / OV7670.c / App.c 一行都不用改
  *            2. 万一以后不想用 RTOS 了, 只要把这个文件换回裸机实现
  ******************************************************************************
  */

#include "Tick.h"
#include "FreeRTOS.h"
#include "task.h"

/* 可选的 1ms 回调, 由 Tick_SetMsCallback() 注册。
   注册/读取都是单条 32 位存取, 不需要临界区。 */
static void (*s_ms_cb)(void) = 0;

/* ========================================================================
 * 毫秒计数
 *
 * ⚠ **调度器启动之前, 这个函数返回恒为 0** —— tick 还没开始跑。
 *   所以不要在 main() 的初始化阶段拿它当时间戳去算超时
 *   (那样 "已经过了 N 毫秒吗" 会永远不成立, 表现为死等)。
 *
 *   移植时审计过: 本工程只有 Led_Init() 在调度器前读了一次, 而且它存下来
 *   只是当"上次翻转的时刻", 值等于 0 和裸机时代完全等价, 无副作用。
 * ====================================================================== */
uint32_t Tick_GetMs(void)
{
  /* configTICK_RATE_HZ = 1000, 所以 portTICK_PERIOD_MS = 1, 乘不乘都一样;
     留着是为了万一以后改了 tick 频率这里不用跟着动。 */
  return (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);
}

void Tick_SetMsCallback(void (*fn)(void))
{
  s_ms_cb = fn;
}

uint8_t Tick_Elapsed(uint32_t start, uint32_t ms)
{
  /* 无符号减法: 计数回绕时差值依然正确, 不用特判 */
  return ((uint32_t)(Tick_GetMs() - start) >= ms) ? 1U : 0U;
}

/* ========================================================================
 * FreeRTOS 的 tick 钩子 —— 每 1ms 被内核调用一次
 *
 * 这是 "Tick_Init() + SysTick_Handler()" 那一套的接替者:
 *   以前 Tick.c 有自己的 SysTick 中断处理函数, 现在由内核的
 *   xPortSysTickHandler 调到这里来(见 FreeRTOSConfig.h 的 configUSE_TICK_HOOK)。
 *
 * ⚠ **在中断上下文里执行**, 所以注册进来的回调只能做很快的活
 *   (读个引脚、置个标志), 绝对不许阻塞 —— 不许调 Usart_SendBytes 这类
 *   会等待的东西, 不许调带 FromISR 之外后缀的 FreeRTOS API。
 *
 * ⚠ 也**不要**在这里调 Tick_GetMs() 或任何可能阻塞的 API: tick 计数在钩子
 *   执行期间还没有递增, 而且上下文不对。当前注册的是 Encoder_SwTick1ms()
 *   (编码器按键采样), 它只读一次 GPIO 再加减计数。
 * ====================================================================== */
void vApplicationTickHook(void)
{
  if (s_ms_cb != 0)
  {
    s_ms_cb();
  }
}
