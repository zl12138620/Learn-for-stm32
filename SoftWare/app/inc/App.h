/**
  ******************************************************************************
  * @file    SoftWare/inc/App.h
  * @brief   应用层: TFT 功能菜单的界面调度
  *
  *          管三件事:
  *            1. 界面状态机 —— 现在在主菜单 / 舵机界面 / 摄像头界面
  *            2. 把编码器输入分发给当前界面
  *            3. 串口回显、开机横幅、临时诊断
  *
  *          **本模块是"胶水层"**: 只有它同时知道 camera / comms / display /
  *          motion 这些领域。那些领域之间互不相识 —— 比如 OV7670.c 不会
  *          include Usart.h, 摄像头开机自检的打印逻辑放在这里就是这个道理。
  *          这样任何一个领域目录都能整块搬到别的工程里。
  *
  *          界面怎么画在 Menu.c(display 领域), 本模块只决定"画哪个"。
  ******************************************************************************
  */

#ifndef __APP_H
#define __APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"

/* ============================ API ============================ */
/* 建三个任务(UiTask / CameraTask / LedTask) + 建 LCD 互斥量。
   ⚠ 在**调度器启动之前**调(main.c 里), 所以它自己不能阻塞、也不该依赖 tick。
   ⚠ 调用前必须先完成: Usart_Init()(要打印)、LCD_Init()(要画)、
     OV7670_Init()(横幅要读它的 ID)、Servo_Init()、Encoder_Init()。
   顺序在 USER/main.c 里, 一眼能看全。

   任务的活都写在 App.c 里, 对外再也不需要别的入口 —— 以前那个
   `App_Run()` 已经变成 UiTask 的私有实现(App_UiRun), 不再暴露。 */
void App_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_H */
