/**
  ******************************************************************************
  * @file    SoftWare/ui/inc/Lvgl_Port.h
  * @brief   LVGL 移植层 —— LVGL 内核和本工程硬件之间的唯一接口
  *
  *          LVGL 是**第三方库**: `LVGL/` 目录下的源码一个字都不改。
  *          所有"LVGL 想知道的外面世界"都从这里喂进去:
  *
  *            1. 时基   —— 一行 lv_tick_set_cb(Tick_GetMs)
  *            2. 显示   —— flush 回调直接复用现成的 LCD_DrawImage()
  *            3. 输入   —— 编码器转/按, 接到 LVGL 的 encoder 设备上
  *
  *          和 FreeRTOS 那套是同一个套路: 内核放顶层不动, 配置和对接
  *          放在 SoftWare/ 的领域里(见 SoftWare/README.md)。
  *
  *          ⚠ 线程模型: lv_conf.h 里 LV_USE_OS 选的是 LV_OS_NONE,
  *            也就是说 **LVGL 没有任何内部锁**。所有 Lvgl_xxx() 都必须在
  *            **同一个任务**里调 —— 目前是 App.c 的 UiTask。
  *            别的任务想往屏幕上画, 得自己跟 UiTask 协调(要么发通知让
  *            UiTask 画, 要么退回去用 display 领域那套 Menu_DrawXxx + 互斥量)。
  ******************************************************************************
  */

#ifndef __LVGL_PORT_H
#define __LVGL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 诊断开关 ============================
   临时排障用, 和 App.c 里的 UI_DIAG 是同一个套路(见那边的说明)。

   打开后, 串口会多出 LVGL 每一步的进度:
     [LVGL] start                 准备初始化
     [LVGL] init ok ...           内核起来了, 附内存池用量
     [LVGL] disp=0x........       显示设备建出来了(全 0 就是失败了)
     [LVGL] display ok ...        显示通路接上了
     [LVGL] demo built ...        演示页控件建完, 附内存用量 + flush 次数
     [LVGL] flush#1 128x40 @0,0   第一次真正往屏上送数据
     [LVGL] t=1000/2000/...       演示循环每秒一次心跳
   外加 LVGL **自己**的日志也会转投到串口(lv_conf.h 第 5 节)。

   为什么需要这些: 第一次烧录时屏幕上什么都没有, 而可能的原因有一长串
   (没初始化 / 显示设备没建成 / 事件没触发 flush / 送了但屏没显示)。
   这些线索靠猜是分不开的, 打出来一眼就能定位到是哪一步断的。

   ⚠ 串口 9600 是**阻塞**发送(约 1ms/字节), 这套输出会让开机多花
     小半秒。排查完把这里改成 0 重新编译, 相关代码就不会被编译进去。

   ⚠ 这个开关只影响**我们自己的**诊断输出; 要连 LVGL 的日志一起关掉,
     还得改 lv_conf.h 的 LV_USE_LOG。 */
#define LVGL_DIAG 1

/* ============================ API ============================ */

/* 初始化 LVGL + 注册显示设备(和输入设备)。
   ⚠ 必须在**调度器起来之后**调: 时基用的是 Tick_GetMs(), 而它底层是
     xTaskGetTickCount(), 调度器没跑的时候那个值永远不动 —— 表现是
     LVGL 所有动画/超时全部纹丝不动, 但界面能画出来, 很迷惑。
   ⚠ 也必须在 LCD_Init() 之后调。
   调用点在 App.c 的 UiTask 里。 */
void Lvgl_Init(void);

/* LVGL 的主心跳, 内部就是 lv_timer_handler()。
   需要被**周期性**调用(本工程是 UiTask 里 5ms 一次), 它负责:
     跑定时器、处理输入、判断哪些区域脏了并调 flush 回调。
   ⚠ 一次调用可能触发多次 flush(画布缓冲小于全屏时, 一屏分几次送),
     而本工程的 flush 是**阻塞**的(等 DMA 送完), 所以这个函数会占用
     真实的 SPI 传输时间 —— 一屏 128x160 在 21MHz 上约 40ms 量级。
     这也是为什么它必须在独立任务里跑, 不能塞进别的地方。 */
void Lvgl_TaskHandler(void);

/* 演示页: 建几个控件验证"显示 + 输入 + 时基"三条通路都通了。
   ⚠ 只在 LVGL_DEMO 打开时编译(App.c 里那个 #if)。
   界面上有个每秒跳动的时钟 —— 它自己在动就说明 lv_timer 和 tick 都通了,
   不用盯着别的现象猜。 */
void Lvgl_DemoShow(void);

#if (LVGL_DIAG != 0)
/* 把 LVGL 当前的内存占用和 flush 次数打到串口, tag 是这行前面的抬头
   (比如 "demo built"), 用来区分是哪一步打的 —— 多打几个点, 连起来
   就能看出内存是在哪一步涨上去的。 */
void Lvgl_DiagReport(const char *tag);

/* 只打一行字(自动加 "[LVGL] " 抬头和换行)。
   给"走到某个分支了"这类事件用, 比如 Lvgl_Demo.c 发现没有可用屏幕。 */
void Lvgl_DiagMsg(const char *msg);
#else
/* 关掉诊断时变成空操作 —— 这样调用方**不用在每处都套一层 #if**,
   插桩代码可以一直留在原地, 只改这一个开关。 */
#define Lvgl_DiagReport(tag)   do { } while (0)
#define Lvgl_DiagMsg(msg)      do { } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __LVGL_PORT_H */
