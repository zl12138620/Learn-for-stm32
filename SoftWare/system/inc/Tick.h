/**
  ******************************************************************************
  * @file    SoftWare/inc/Tick.h
  * @brief   1ms 毫秒时基(SysTick) —— 给按键消抖和帧率统计用
  *
  *          为什么非要它不可:
  *            摄像头界面主循环一轮约 95ms(得等相机把一整帧写完)。任何"在
  *            主循环里数次数"的定时在这里都不成立 —— 数 8000 次要几百秒。
  *            所以按键消抖和帧率统计都必须挂在一个**不受主循环快慢影响**
  *            的时基上, 这就是它。
  *
  *          1ms 中断里干两件事:
  *            1. 累加毫秒计数
  *            2. 调 Encoder_SwTick1ms() 采样编码器 SW 按键
  *
  *          SysTick_Config() 会把 SysTick 中断优先级设成最低(15),
  *          不会抢占编码器 EXTI(4) 和 TIM7(5)。
  *
  *          ⚠ 工程里再也没有别的东西能碰 SysTick 了。曾经有个
  *            SoftUSART.c 的 TX_TickStart() 会直接写 SysTick->CTRL/LOAD/VAL,
  *            一旦被调用就会把这里的心跳当场掐死且不报任何错 —— 那个文件
  *            已经删掉了(需要时从 git 历史找回)。以后要加软串口之类的东西,
  *            请另找定时器, 别再动 SysTick。
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
void     Tick_Init(void);        /* 配置并启动 SysTick 1ms 中断 */

uint32_t Tick_GetMs(void);       /* 读毫秒计数(上电起算, 约 49.7 天回绕) */

/* ---- 1ms 回调注册 ----
   给那些"必须每毫秒采一次、不能等到主循环"的外设用。目前只有编码器的
   按键消抖(Encoder)在用它 —— 采晚了会漏掉快速点按。

   ⚠ 回调在**中断上下文**里执行, 只能做很快的活(读个引脚、置个标志),
     绝对不许阻塞: 不许调 Usart_SendBytes 这类会等待的东西, 不许延时,
     否则其他中断全被拖住。

   为什么要做成回调而不是让 Tick 直接调 Encoder:
     那样 system/ 就得 include motion/Encoder.h —— **底层依赖上层**, 方向反了。
     反转成"上层主动注册"之后, Tick 不认识任何人, system/ 保持自包含,
     可以整块搬到别的工程。传 NULL 取消注册。 */
void     Tick_SetMsCallback(void (*fn)(void));

/* "从 start 到现在过了 ms 毫秒了吗"。内部用无符号差值, 计数回绕也安全。
   直接写 (Tick_GetMs() - start) >= ms 是一样的, 封装起来是为了意图清楚,
   也省得每处都手写括号写错。 */
uint8_t  Tick_Elapsed(uint32_t start, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __TICK_H */
