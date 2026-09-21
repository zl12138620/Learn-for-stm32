/**
  ******************************************************************************
  * @file    SoftWare/system/inc/Tick.h
  * @brief   毫秒时基 —— FreeRTOS 接管 SysTick 之后的包装层
  *
  *          为什么非要有个时基不可:
  *            摄像头界面一轮约 95ms(得等相机把一整帧写完)。任何"在主循环里
  *            数次数"的定时在这里都不成立 —— 数 8000 次要几百秒。所以按键
  *            消抖和帧率统计都必须挂在一个**不受主循环快慢影响**的时基上。
  *
  *          ---- 2026-09-22 移植 FreeRTOS 后的变化 ----
  *            以前: 本模块自己写 SysTick_Handler, 自己 SysTick_Config, 自己
  *                  累加 s_tick_ms。
  *            现在: **SysTick 归内核**, 它的心跳就是 FreeRTOS 的 tick。
  *                  本模块只剩一层包装:
  *                    Tick_GetMs()  -> xTaskGetTickCount()
  *                    1ms 按键采样  -> FreeRTOS 的 tick 钩子
  *                                     (vApplicationTickHook, 实现已挪到 Tick.c)
  *                  `Tick_Init()` 已被删除 —— 调度器启动时 port 自己会配好 SysTick。
  *
  *          这么包一层的好处: Led.c / OV7670.c / App.c 一行都不用改;
  *          哪天不想用 RTOS 了, 把这个文件和 Tick.c 换回裸机实现即可。
  *
  *          ⚠ **调度器启动之前 `Tick_GetMs()` 恒返回 0**(tick 还没开始跑)。
  *            别在 main() 的初始化阶段拿它当时间戳判超时, 那会永远不成立。
  ******************************************************************************
  */

#ifndef __TICK_H
#define __TICK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ API ============================ */
/* 读毫秒计数(从调度器启动算起, 约 49.7 天回绕)。
   ⚠ 调度器启动前恒为 0, 见文件头的说明。 */
uint32_t Tick_GetMs(void);

/* "从 start 到现在过了 ms 毫秒了吗"。内部用无符号差值, 计数回绕也安全。
   直接写 (Tick_GetMs() - start) >= ms 是一样的, 封装起来是为了意图清楚,
   也省得每处都手写括号写错。 */
uint8_t  Tick_Elapsed(uint32_t start, uint32_t ms);

/* ---- 1ms 回调注册 ----
   给那些"必须每毫秒采一次、不能等到任务调度"的外设用。目前只有编码器的
   按键消抖(Encoder)在用它 —— 采晚了会漏掉快速点按。

   ⚠ 回调**在中断上下文里执行**(由 FreeRTOS 的 tick 钩子调进来),
     只能做很快的活(读个引脚、置个标志), 绝对不许阻塞:
     不许调 Usart_SendBytes 这类会等待的东西, 不许延时,
     也不许调任何没带 FromISR 后缀的 FreeRTOS API。

   为什么要做成回调而不是让 Tick 直接调 Encoder:
     那样 system/ 就得 include motion/Encoder.h —— **底层依赖上层**, 方向反了。
     反转成"上层主动注册"之后, Tick 不认识任何人, system/ 保持自包含,
     可以整块搬到别的工程。传 NULL 取消注册。 */
void     Tick_SetMsCallback(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* __TICK_H */
