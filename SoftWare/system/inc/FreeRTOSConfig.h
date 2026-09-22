/**
  ******************************************************************************
  * @file    SoftWare/system/inc/FreeRTOSConfig.h
  * @brief   FreeRTOS 内核配置 —— **本工程唯一的 FreeRTOS 配置入口**
  *
  *          内核源码在顶层 FreeRTOS-Kernel/(第三方, 不要改), 要调行为就改这里。
  *          FreeRTOS.h 会自动 #include "FreeRTOSConfig.h", 所以这个文件必须
  *          在编译器的头文件搜索路径里 —— 它在 SoftWare/system/inc/, 已包含。
  *
  *          2026-09-22 移植时的三个关键决定(踩过, 别改回去):
  *
  *          1) **三个异常向量的别名**(文件最后那组 #define)
  *             port.c 提供的是 vPortSVCHandler / xPortPendSVHandler /
  *             xPortSysTickHandler 这三个名字, 必须接到 CMSIS 的
  *             SVC_Handler / PendSV_Handler / SysTick_Handler 上。
  *             而工程里这三处原来是**有强定义的**(System/stm32f4xx_it.c 里两个、
  *             SoftWare/system/src/Tick.c 里一个), 已经删掉 —— 不删就
  *             链接报 multiple definition。
  *
  *          2) **中断优先级**(看下面 configLIBRARY_ 那组)
  *             STM32F4 只用 4 位优先级, 但 NVIC 的优先级寄存器是 8 位宽,
  *             所以这些"库级"数值要左移 (8 - configPRIO_BITS) 才能写进寄存器。
  *             任何会调用 ...FromISR() 的中断, 抢占优先级必须 **数值上 ≥ 5**,
  *             否则 vPortValidateInterruptPriority() 的 configASSERT 会命中。
  *
  *          3) **NVIC 优先级分组必须显式设**
  *             FreeRTOS 要求 4 位全给抢占优先级(即 NVIC_PriorityGroup_4)。
  *             本工程之前**从没调用过 NVIC_PriorityGroupConfig()**, 而 StdPeriph
  *             的 NVIC_Init() 是按 AIRCR.PRIGROUP 现算的 —— PRIGROUP 为复位值时
  *             算出来的 IPR 是 0x00, 源码里写的优先级数字**被整体丢弃**。
  *             现在 USER/main.c 里在启动调度器前显式设置了。
  ******************************************************************************
  */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ============================ 调度器 ============================ */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1   /* Cortex-M4 有 CLZ 指令, 用得上 */
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      (168000000UL)
#define configTICK_RATE_HZ                      1000        /* 1ms 一个 tick */
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                128         /* 单位是"字", 不是字节 */
#define configMAX_TASK_NAME_LEN                 10
#define configUSE_16_BIT_TICKS                  0           /* 32 位 tick, 49 天才回绕 */
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1           /* 同优先级轮转; 本工程没用到, 留着不碍事 */

/* ============================ 内存 ============================ */
/* heap_4, 24KB。
   2026-09-22 从 16KB 提到 24KB: 引入 LVGL 之后 UiTask 的栈需求大涨
   (3KB 实测溢出, 见 App.c 里 UI_TASK_STACK 的说明)。
   各任务栈加起来约 11.6KB, 24KB 留了 2 倍余量。
   真不够了会调到 vApplicationMallocFailedHook(), 不会悄悄失败。 */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (24 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ============================ 同步原语 ============================ */
#define configUSE_TASK_NOTIFICATIONS            1   /* UiTask -> CameraTask 靠它唤醒 */
#define configUSE_MUTEXES                       1   /* 保护 LCD, 见 Menu.c */
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0

/* ============================ 钩子(教学脚手架) ============================ */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     1   /* ⚠ 编码器按键采样挂在这, 见 Tick.c */
#define configCHECK_FOR_STACK_OVERFLOW          2   /* 任务栈溢出立刻抓出来, 别等"系统莫名卡死" */
#define configUSE_MALLOC_FAILED_HOOK            1   /* 堆不够时报警, 而不是建任务静默失败 */
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
/* 两个钩子函数在 SoftWare/app/src/App.c 里实现(那里能同时用串口和 LED) */

/* ============================ 运行统计 / 追踪(都关) ============================ */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ============================ 软件定时器 ============================ */
/* 不用。省下一个服务任务的栈和一块 RAM。 */
#define configUSE_TIMERS                        0

/* ============================ 其它功能开关 ============================ */
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES 0

/* ============================ 中断优先级 ============================ */
/* STM32F4 实现 4 位优先级; 数值越小优先级越高。 */
#define configPRIO_BITS                              4

/* 最低优先级: 给 SysTick/PendSV 这些内核中断用 */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15

/* ⚠ 分界线: 抢占优先级"数值 ≥ 这个值"的中断, 才允许调用 ...FromISR()。
   高于它的中断(数值更小)不会被内核的临界区屏蔽, 也就绝不能在里面调 FreeRTOS API。
   本工程里 EXTI9_5 / TIM7 / USART1 都设成了 5。 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

/* 左移到 NVIC 的 8 位寄存器的高 4 位 */
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ============================ 断言 ============================ */
/* 条件不成立就关中断死循环 —— **停在现场**。
   接上调试器能看到调用栈和出错的表达式, 比打印一行日志有用得多;
   而且断言可能在串口初始化之前就触发, 那时候打印会死等, 反而更难查。 */
#define configASSERT(x) \
    if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) { } }

/* ============================ 异常向量别名 ============================ */
/* 把 port.c 里的三个处理函数接到 CMSIS 启动文件用的名字上。
   ⚠ 工程里原来 SVC_Handler / PendSV_Handler 在 System/stm32f4xx_it.c、
     SysTick_Handler 在 SoftWare/system/src/Tick.c, **都已经删掉了** ——
     同一个向量定义两处会链接报 multiple definition。 */
#define vPortSVCHandler         SVC_Handler
#define xPortPendSVHandler      PendSV_Handler
#define xPortSysTickHandler     SysTick_Handler

/* ============================ INCLUDE_ 开关 ============================ */
/* 默认全是 0, 用到哪个开哪个 —— 关着的不参与编译, 省 FLASH。 */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1   /* 看任务栈还剩多少, 调栈大小时用 */
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_eTaskGetState                   1

#endif /* FREERTOS_CONFIG_H */
