/**
  ******************************************************************************
  * @file    SoftWare/src/App.c
  * @brief   应用层: TFT 功能菜单的界面调度 —— 详见 App.h 的说明
  ******************************************************************************
  */

#include "App.h"
#include "Usart.h"
#include "Menu.h"
#include "Servo.h"
#include "Encoder.h"
#include "OV7670.h"
#include "LCD.h"
#include "Led.h"
#include "Tick.h"
#include "Lvgl_Port.h"
#include "Ui_Screens.h"
#include "Ui_Camera.h"

#include "FreeRTOS.h"
#include "task.h"

/* ======================= 任务参数 ======================= */
/* 栈的单位是**字(4 字节)**, 不是字节 —— 512 字 = 2KB。
   给得宽松一点, 因为栈溢出的后果是"系统莫名卡死", 很难查;
   configCHECK_FOR_STACK_OVERFLOW 会在溢出时立刻报出来, 但那是兜底。 */
/* 栈的单位是**字(4 字节)**, 不是字节 —— 512 字 = 2KB。
   两个画屏的任务给得宽一点: LCD 那套调用嵌得比较深, 而栈溢出的后果是
   "系统莫名卡死"很难查。configCHECK_FOR_STACK_OVERFLOW 会立刻报出来,
   但那是兜底, 不是省栈的理由。 */
/* ⚠⚠ 2026-09-22 引入 LVGL 时由 512 提到 2048(= 8KB)。这段经过值得留着:
 *
 *   512 -> 768 是我第一次"按感觉加一点"。**不够**, 第一次烧录当场翻车:
 *   串口打出 [RTOS] 栈溢出! 而且从头到尾**一次 flush 都没完成** ——
 *   说明它死在第一次 lv_timer_handler() 里面, 也就是 LVGL 的绘制路径。
 *
 *   768(3KB) 为什么差这么多: LVGL 画一屏的调用链比手写菜单深得多 ——
 *     lv_timer_handler -> 刷新定时器 -> lv_refr_area -> 逐层绘制 ->
 *     软件混合 -> 取字形位图 ...
 *   中间还有按绘制内容动态决定的栈上局部量, 所以**深度不是常数**,
 *   只能量, 不能估。
 *
 *   现在给 2048(8KB), 并且下面每秒把 uxTaskGetStackHighWaterMark() 打出来
 *   ——它给的是"历史上最少剩过多少字", 跑一会儿就知道真实需求,
 *   到时候按实测往回收。**别再按感觉加了**。
 *
 *   代价: 8KB 从 FreeRTOS 堆里出, 所以 FreeRTOSConfig.h 的
 *   configTOTAL_HEAP_SIZE 同步从 16KB 提到了 24KB。
 *   等哪天真不用 LVGL 了, 这里可以改回 512。 */
#define UI_TASK_STACK       2048U
#define UI_TASK_PRIO        (tskIDLE_PRIORITY + 4)

#define CAM_TASK_STACK      512U
#define CAM_TASK_PRIO       (tskIDLE_PRIORITY + 2)

#define LED_TASK_STACK      192U
#define LED_TASK_PRIO       (tskIDLE_PRIORITY + 1)

/* UiTask 的轮询周期。编码器/按键都是**事件锁存**的(采样在 1ms 的 tick 钩子里,
   见 Tick.c), 所以这里晚几毫秒不会漏按键, 只影响手感。
   但**必须让出 CPU** —— prio 4 的任务若空转, 会把下面的 CameraTask(2) 和
   LedTask(1) 一起饿死。这是任务拆分里最容易踩的一脚。 */
#define UI_POLL_MS          5U

/* UiTask 超过这么久没动静就认定它卡住了, LedTask 转成快闪报警。
   ⚠ 给到 2 秒而不是 1 秒, 是为了躲开一个**误报**: UiTask 里的串口回显是
     阻塞发送的(9600 下 1.04ms/字节), 往串口猛灌数据时它会一次卡住好几百
     毫秒 —— 那是正常的, 不该当成故障。真要看回显阻塞的问题, 得把
     发送改成中断/流缓冲, 不是靠调这个阈值。 */
#define UI_ALIVE_TIMEOUT_MS 2000U

static void UiTask(void *arg);
static void CameraTask(void *arg);
static void LedTask(void *arg);

/* CameraTask 的句柄: UiTask 靠任务通知唤醒它(界面切进/切出摄像头界面)。
   CameraTask 不在摄像头界面时正阻塞在这个通知上, 零 CPU 占用。 */
static TaskHandle_t s_cam_task = NULL;

/* UiTask 的存活计数, 只增不减。LedTask 靠它判断 UiTask 还在不在。
   ⚠ RTOS 里"灯在闪"不再等于"整个系统活着", 只等于 LedTask 自己还在跑 ——
     要把这个判断显式做出来, 否则会白白丢掉一个很好用的故障指示。
   volatile + 32 位单次读写 = 原子, 不需要加锁。 */
static volatile uint32_t s_ui_alive = 0U;

/* ======================= 临时诊断开关 =======================
   排查编码器接线用。开着时串口会多出:
     A=1 B=0 SW=1   三根线的电平, 任何一根变了就打一行
     [SW]           检测到一次"按下"
     [ROT +2]       转了 +2 格(负数是反转)

   ⚠ 会刷屏, 属于预期。排查完把这里改成 0 重新编译, 诊断代码就不会被编译进去。
   (2026-09-22 靠它定位到"编码器公共端 C 没接地" —— 转一整圈 A/B 从没到过 0
    就是那个症状。详见 学习笔记/ 或记忆里的引脚表。)

   编码器问题已解决、重构也验收过了, 所以关掉。以后要查编码器接线
   再改回 1 重新编译即可。 */
#define UI_DIAG  0

/* ======================= LVGL 演示页开关 =======================
   2026-09-22 引入 LVGL 的第一步: **只做验证通路, 不替换任何现有代码**。

   开机横幅之后、进主菜单之前, 先跑一段独立的 LVGL 演示页, 到时间就
   照常进现在这套菜单(菜单那边会 LCD_Clear 掉, 互不干扰)。

   为什么要这么试, 而不是直接把菜单改成 LVGL:
     1. 现在这套菜单是刚验证过的 500 行工作代码。万一是 LVGL 在 128x160
        这种小屏上效果不理想, **把这里改回 0 就完全回到现状** ——
        Menu.c / CN_Font.c / Font8x16.c 一行都没动过。
     2. 屏只有 128x160, LVGL 的控件是按 320x240 设计的, 一个默认按钮就
        占掉半个屏宽。**好不好看只有烧上去才知道**, 光看文档没用。

   改回 0 之后 LVGL 就不会被编译进镜像了吗? **不是** —— 源码还是会被编译
   (CMakeLists.txt 里整棵 src/ 都收了), 只是没人调用, 链接期 --gc-sections
   会把它全部丢掉。所以关掉之后 flash 占用和没移植时一样。

   ⚠ 打开时 UI_TASK_STACK 给到了 768(见上面), 关掉后可以改回 512。 */
/* ⚠ 2026-09-22 改成 0: 正式界面已经建好了, 演示页的使命结束。
   关掉它有三个实际好处, 不只是"省 8 秒":
     1. 演示页那 6 个控件会**一直占着** LVGL 的内存池(约 2KB) —— 它建的控件
        从来没被删过, 而它所在的屏幕切走之后依然活着;
     2. 演示页那个时钟定时器**还在后台跑**(每 500ms 一次), 一直在池子上
        分配/释放字符串, 属于持续的碎片来源;
     3. 排查"正式界面卡死"时, 少一个变量参与。
   要回去看演示页把它改成 1 即可, Lvgl_Demo.c 一行没动。 */
#define LVGL_DEMO     0
#define LVGL_DEMO_MS  8000U     /* 演示持续时长(毫秒) */

/* ======================= 正式界面用 LVGL =======================
   2026-09-22: 把三个界面(主菜单/摄像头/舵机)从手写的 display/Menu.c
   换成 LVGL 控件。代码在 SoftWare/ui/ 的 Ui_Screens.c + Ui_Camera.c。

   ⚠ 这个开关和上面的 LVGL_DEMO 是**两件事**:
     LVGL_DEMO 是"开机跑 8 秒演示页", 纯粹验证通路, 留着;
     UI_LVGL    是"用 LVGL 当正式界面"。
     两个都开着时的顺序是: 演示页跑 8 秒 -> 进 LVGL 主菜单。

   改回 0 会发生什么: App_UiRun / UI_Enter 走回 Menu_DrawXxx 那条老路,
   Menu.c / Menu.h / CN_Font.c / Font8x16.c **一行都没动过**, 完全回到现状。
   (那三个老函数连同它们的声明会被 #if 排除掉, 不参与编译,
     链接期再由 --gc-sections 把没人用的 Menu.c 丢掉。)

   ⚠ 关掉之后 UI_TASK_STACK 可以往回收, 但**先量**: 手写菜单那条路栈需求
     低得多, 但别忘了我量的是 LVGL 那条路的值。 */
#define UI_LVGL       1

/* ======================= 界面状态机 ======================= */
typedef enum
{
    UI_MAIN_MENU = 0,       /* 主菜单 */
    UI_CAMERA,              /* 摄像头(OV7670 + FIFO) */
    UI_SERVO                /* 舵机(SG90) */
} ui_screen_t;

/* 编码器每转一小格, 舵机走多少度。一格 5° 的话 36 格走完全程 180°, 手感合适。 */
#define SERVO_STEP_DEG  5

/* ---- 共享状态: 谁写谁读, 标在每条后面 ----
   ⚠ s_screen 是**唯一**被两个任务碰到的东西(UiTask 写, CameraTask 读),
     所以它必须是 volatile。单个 32 位字的读写是原子的, 不需要加锁;
     真正需要小心的是"读了它之后要做什么", 那部分靠 LCD 互斥量兜
     (见 Menu_DrawCameraFrameLocked 的注释)。 */
static volatile ui_screen_t s_screen = UI_MAIN_MENU;   /* UiTask 写 / CameraTask 读 */

#if (UI_LVGL == 0)
static uint8_t     s_sel    = 0U;       /* 主菜单当前选中项 —— 只有 UiTask 碰 */
#endif

/* 舵机角度的**权威值**。只有 UiTask 碰。
   ⚠ 为什么 App.c 存一份而 Ui_Screens.c 里也有一份: Ui_Screens 那份只是
     "屏幕上画着什么"的缓存(进界面时会被 UiScreens_SetAngle 覆盖),
     这一份才是真值 —— 它决定真实的 PWM。以硬件为准, 不以显示为准。 */
static uint8_t     s_angle  = 90U;      /* 上电回中位 */

/* ---- LVGL 界面的动作队列 ----
   ⚠ 这是这套界面能安全工作的关键, 不是多余的一层:

     界面回调(Ui_Screens.c 里那些 Ui_xxxCb)是在 **LVGL 的事件派发内部**被调的。
     在那里直接调 lv_screen_load() 等于"处理某个控件的事件处理到一半,
     把它所在的整个界面换掉" —— 典型的自伤操作(LVGL 正拿着旧界面的对象指针
     往下走)。

     所以回调只**登记意图**到这两个变量, 真正的切换放到 App_UiRun 里做 ——
     那时已经退出了 LVGL 的派发, 想怎么换屏都行。

   ⚠ 只排队"换界面"这一类动作。舵机角度不排队(见 App_ActionCb 的说明),
     所以这里不用存参数。单字节读写是原子的, 只有 UiTask 碰它。 */
static volatile uint8_t s_pending_act = 0xFFU;   /* 0xFF = 没有待处理动作 */

/* 帧率统计: 数 1 秒窗口内抓到几帧。s_frames/s_fps_t0 只有 CameraTask 碰。 */
static uint32_t s_frames = 0U;
static uint32_t s_fps_t0 = 0U;

/* ⚠ s_fps 是**跨任务**的: CameraTask 写, UiTask 读(拿去更新界面上的帧率数字)。
   所以必须是 volatile —— 不加的话编译器有权把这个值缓存在寄存器里跨循环复用,
   于是界面上的帧率会一直停在某个旧值上。单字节读写本身是原子的, 不需要加锁。 */
static volatile uint8_t s_fps = 0U;

/* ======================= 函数声明 ======================= */
static void UI_Enter(ui_screen_t s);
static void App_UiRun(void);
static void App_SendCamId(void);
static void App_ActionCb(ui_action_t act, uint8_t arg);

#if (UI_LVGL == 0)
/* ---- 手写界面那条路(App.c 里原来那三个函数) ----
   只在 UI_LVGL = 0 时编译。留着是为了"开关改回 0 就完全回到现状"。
   注意 display/Menu.c 本身**没有**被 #if 掉, 它一直都能编译 ——
   这三个函数不用它的时候, 链接期 --gc-sections 会把没人引用的部分丢掉。 */
static void UI_MainMenu(int32_t delta, uint8_t pressed);
static void UI_Servo(int32_t delta, uint8_t pressed);
static void UI_Camera(uint8_t pressed);
#endif

#if (UI_DIAG != 0)
static void Diag_PrintLevels(void);
#endif

#if ((LVGL_DEMO != 0) || (UI_LVGL != 0))
static void Diag_PrintStackFree(void);
#endif

/* ========================================================================
 * 开机
 *
 * ⚠ 这个函数在**调度器启动之前**被调用(main.c 里), 所以它只负责建任务,
 *   不能有阻塞或依赖 tick 的操作 —— 那要放到任务里去。
 * ====================================================================== */
void App_Init(void)
{
    /* ⚠ 互斥量必须在**建任务之前**创建 —— CameraTask 一起来就可能画屏 */
    Menu_Init();

    /* 注册界面动作回调。
       ⚠ 这里只是存一个函数指针, 所以可以在调度器之前调。
         真正的界面控件要等 LVGL 起来了才能建(UiScreens_Init 在 UiTask 里)。 */
    UiScreens_SetActionCb(App_ActionCb);

    /* 建失败会调到 vApplicationMallocFailedHook(堆不够), 不会静默过去 */
    (void)xTaskCreate(UiTask,     "Ui",  UI_TASK_STACK,  NULL, UI_TASK_PRIO,  NULL);
    (void)xTaskCreate(CameraTask, "Cam", CAM_TASK_STACK, NULL, CAM_TASK_PRIO, &s_cam_task);
    (void)xTaskCreate(LedTask,    "Led", LED_TASK_STACK, NULL, LED_TASK_PRIO, NULL);
}

#if ((LVGL_DEMO != 0) || (UI_LVGL != 0))
/* ========================================================================
 * 打一次当前任务的栈余量
 *
 *   uxTaskGetStackHighWaterMark(NULL) 返回**当前任务**历史上"最少还剩多少"
 *   (单位是字, 4 字节)。是历史最低点, 不是当前值 —— 所以这个数只会变小,
 *   记录的是最坏情况, 正是我们想要的。
 *
 *   ⚠ **越小越危险**: 接近 0 就说明栈快不够了。要给多少才安全没有公式,
 *     经验上留 1/4 以上余量比较稳(即 HWM 别低于总量的 25%)。
 *
 *   为什么非得量: 见上面 UI_TASK_STACK 那段 —— LVGL 的绘制深度随内容变化,
 *   估算两次都偏差很大。这个数跑几秒就出来了, 比任何估算都硬。
 * ====================================================================== */
static void Diag_PrintStackFree(void)
{
    static const char P1[] = "[STACK] free=";
    static const char P2[] = " words\r\n";

    char        b[10];
    uint8_t     n = 0U;
    uint32_t    v = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    Usart_SendBytes((const uint8_t *)P1, sizeof(P1) - 1U);

    if (v == 0U)
    {
        Usart_SendBytes((const uint8_t *)"0", 1U);
    }
    else
    {
        while ((v > 0U) && (n < 10U))
        {
            b[n] = (char)('0' + (v % 10U));     /* 低位先出来, 回头倒着发 */
            v /= 10U;
            n++;
        }
        while (n > 0U)
        {
            n--;
            Usart_SendBytes((const uint8_t *)&b[n], 1U);
        }
    }

    Usart_SendBytes((const uint8_t *)P2, sizeof(P2) - 1U);
}
#endif /* LVGL_DEMO || UI_LVGL */

/* ========================================================================
 * UiTask —— 按键/编码器 → 状态机 → 菜单和舵机界面的绘制 + 串口回显
 *
 * 移植 FreeRTOS 的**全部收益**就在这个任务上: 以前整个超级循环被相机抓帧
 * 占住, 按一下 SW 最多要等 95ms(等一帧 + 读 FIFO + 送屏)才被处理;
 * 现在抓帧在 CameraTask 里, 这个任务独立跑, 按键几乎立刻响应。
 * ====================================================================== */
static void UiTask(void *arg)
{
    (void)arg;

    App_SendCamId();                /* 横幅: 摄像头自检结果, 接好线后第一个该看的 */

#if (LVGL_DEMO != 0)
    /* ---- LVGL 演示页(临时, 见文件顶部的 LVGL_DEMO 开关) ----
       ⚠ 这里**不需要**去抢 LCD 互斥量: 演示期间 s_screen 一直是
         UI_MAIN_MENU(还没调 UI_Enter), 而 CameraTask 只要不处于摄像头界面
         就阻塞在任务通知上(见 CameraTask 开头), 根本碰不到屏。
         所以此刻屏幕是 LVGL 独占的 —— 这一步先把通路验通, 并发问题
         留到"真要把整套菜单换成 LVGL"的时候再处理。
       ⚠ 演示期间摄像头不工作(界面不在摄像头那一页), 演示结束就恢复。

       ⚠⚠ 一个不那么明显、但**必须知道**的耦合:
         LVGL 的定时器是靠 Lvgl_TaskHandler()(内部 lv_timer_handler)驱动的,
         下面这个 while 一退出, **LVGL 就整个停摆了** —— 包括它那个每 33ms
         去读一次编码器的输入定时器。
         这恰好是我们要的: 演示一结束, 编码器就重新归 App_UiRun() 独占,
         不会出现"LVGL 和菜单状态机抢 Encoder_ReadDelta()"的情况
         (那个函数是读走即清的, 两边抢的话会各拿到一半格数, 表现为转动丢步)。
         紧接着的 UI_Enter(UI_MAIN_MENU) 还会把演示期间攒下的格数和按下
         一次性冲掉(见 UI_Enter 开头), 所以残留不会漏进菜单。

         但反过来说: **以后真要全换成 LVGL, 这个"停止调用就停摆"的假设就没了**,
         那时 LVGL 会和菜单状态机同时活着, 抢占编码器的问题就必须正面处理
         (让其中一方独占, 或者改成事件驱动)。这一步先不做, 因为现在还没有
         第二个消费者。 */
    Lvgl_Init();
    Lvgl_DemoShow();
    {
        uint32_t t0 = Tick_GetMs();

        while (Tick_Elapsed(t0, LVGL_DEMO_MS) == 0U)
        {
            /* ⚠⚠ 这一行**不能省**。LedTask 是靠 s_ui_alive 不再变化来判断
               "UiTask 卡住了"的(超时 UI_ALIVE_TIMEOUT_MS = 2 秒), 而这个
               演示循环在下面那个 for(;;) **外面** —— 第一版就是漏了这一行,
               于是开机后前 8 秒 LED 一直在快闪报警, 看着像"系统没起来"。
               现象会骗人: 那 8 秒里 UiTask 其实活得好好的。 */
            s_ui_alive++;

            Lvgl_TaskHandler();

            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* 跑完报一次。栈余量是**历史最低点**, 所以这一次就把整个演示期间
           的最坏情况带出来了, 不用每秒刷屏。
           这个数决定 UI_TASK_STACK 给多少合适 —— 见它的说明。 */
        Diag_PrintStackFree();
    }
#endif

#if (UI_LVGL != 0)
    /* ---- 正式界面: 建三个界面(主菜单 / 摄像头 / 舵机)的控件树 ----
       ⚠ 必须在 UI_Enter() 之前 —— 那一步会 lv_screen_load 到主菜单, 控件得先存在。
       ⚠ lv_init() 内部有"只初始化一次"的守卫, 所以即使上面的演示页已经调过,
         这里再调也是空操作。写在这里是为了照顾 "UI_LVGL 开、LVGL_DEMO 关"
         那种组合 —— 那时上面整块都不编译, LVGL 就得靠这里起来。 */
    Lvgl_Init();
    UiScreens_Init();               /* 建主菜单 + 舵机屏 */

    /* 摄像头界面**单独建**: 它需要帧缓冲的地址, 而那是 camera 领域的东西。
       ⚠ ui 领域**不认识** OV7670(见 SoftWare/README.md 的依赖表:
         ui -> comms display motion system, 没有 camera)。
         本函数是胶水层, 是唯一同时认识两边的地方, 所以由这里把指针递过去。
       ⚠ 探不到摄像头就传 NULL —— 界面会只显示"无信号"。
         检查放在这里和放在进界面时是等价的: OV7670_IsPresent() 的值是
         开机自检那一次锁存的, 运行期不会变。 */
    UiCam_Build((OV7670_IsPresent() != 0U) ? OV7670_GetFrameBuf() : NULL);

    UI_Enter(UI_MAIN_MENU);

    /* 三个界面都建完了, 报一次内存池和栈的用量。
       ⚠ 这两行是**长期保留**的, 不是临时插桩 —— 它们回答"改完界面之后
         池子和栈还够不够", 而这两个数只能量不能估(见 lv_conf.h 第 1 节
         和第 6 节记的两次教训)。串口上就多两行, 换来随时能判断余量。
       ⚠ 定位那次"演示页跑完就死"用的**分级打点**(每步前后各一行那种)已经
         删掉了 —— 它只在"不知道死在哪个调用里"时才有价值。 */
    Lvgl_DiagReport("ui built");
    Diag_PrintStackFree();
#endif

    UI_Enter(UI_MAIN_MENU);

    for (;;)
    {
        s_ui_alive++;               /* 给 LedTask 看的存活心跳(见 LedTask 注释) */

        App_UiRun();

#if (UI_LVGL != 0)
        /* LVGL 的心跳: 跑定时器、读输入(编码器)、刷新脏区域。
           ⚠ 放在 App_UiRun 之后 —— 那里可能刚切了界面, 这一步正好把新界面画出来。
           ⚠ 一次调用可能触发多次 flush(画布缓冲只有 1/4 屏, 一屏分 4 次送),
             而本工程的 flush 回调是阻塞的, 所以这一句会占掉真实的 SPI 传输时间
             (整屏约 15.6ms)。这也是它必须在独立任务里跑的原因。 */
        Lvgl_TaskHandler();
#endif

        /* ⚠ 必须让出 CPU: 这个任务 prio 4, 空转会饿死 CameraTask(2) 和
           LedTask(1)。5ms 的轮询周期对按键手感毫无影响 ——
           按键本身是 1ms tick 在采样并锁存的, 不会漏。
           (真要再快点可以让编码器 EXTI 直接 xTaskNotifyGive 本任务,
            那就是完全事件驱动了, 目前没必要。) */
        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
}

/* ========================================================================
 * CameraTask —— 只在摄像头界面时抓帧 + 送屏
 *
 * 不在摄像头界面时**阻塞在任务通知上, 零 CPU 占用**; UiTask 切换界面时
 * 用 xTaskNotifyGive 把它叫醒(见 UI_Enter 末尾)。
 * ====================================================================== */
static void CameraTask(void *arg)
{
    (void)arg;

    for (;;)
    {
        /* 不在摄像头界面: 挂起等通知。
           (如果进来之前已经有挂起的通知, 这里会立刻返回, 所以下面要再判一次) */
        if (s_screen != UI_CAMERA)
        {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* 开机就没探到摄像头: 画面停在"无信号"不动, 别空转烧 CPU */
        if (OV7670_IsPresent() == 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (OV7670_CaptureFrame() == 0U) { continue; }

        /* ⚠ 写缓冲之前先等渲染栅栏。
           原因: UiTask 渲染一帧时正在从 s_frame 里 memcpy(约 15ms),
           这期间被我们覆写就会撕裂画面(上半帧旧、下半帧新)。
           栅栏由 UiTask 在渲染期间置起, 见 UiCam_IsRendering()。
           ⚠ 本任务 prio 2 < UiTask 的 4, 所以让出 1 个 tick 就足够它跑完;
             而且抓帧周期约 95ms、渲染约 15ms, 大多数时候栅栏本来就是开的,
             这里几乎不会真的等。 */
        while (UiCam_IsRendering() != 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        OV7670_ReadFrameRotated();

        /* 抓帧这 95ms 里用户可能已经退出摄像头界面了 —— 那就别通知了。

           ⚠⚠ 和以前比, 这里**从"锁外判断"变成了安全的**, 值得说清楚为什么:
             以前相机帧是**直接往屏上画**的, 所以"判断"和"画"必须原子做完,
             否则会有"判断完还在摄像头界面、等拿到锁时已经切走了"的窗口,
             那一帧就盖到菜单上了 —— 所以才要 Menu_Lock 那一套。
             现在相机帧**不画到屏上**, 只是把序号往前推一格。UiTask 醒来后会
             再查一次 s_screen, 查不到就什么都不做。
             **没有人会盖掉别人的画面**, 锁自然就不需要了。 */
        if (s_screen != UI_CAMERA) { continue; }

        /* ---- 帧率: 数满 1 秒算一次 ---- */
        s_frames++;
        if (Tick_Elapsed(s_fps_t0, 1000U) != 0U)
        {
            s_fps    = (s_frames > 99U) ? 99U : (uint8_t)s_frames;
            s_frames = 0U;
            s_fps_t0 = Tick_GetMs();
        }

        /* 告诉界面"有新帧了"。
           ⚠ 这一步**不碰任何 LVGL 状态** —— 它是这个任务唯一能做的 LVGL 相关动作。
             LV_USE_OS = LV_OS_NONE, LVGL 没有任何内部锁, 所以所有 lv_* 必须在
             UiTask 里调; 这里只是把一个 volatile 序号 +1(单字读写是原子的)。
             真正的 invalidate + 渲染由 UiTask 的 UiCam_Pump() 做。 */
        UiCam_MarkDirty();
    }
}

/* ========================================================================
 * LedTask —— 心跳 + UiTask 存活监视
 *
 * ⚠ 语义上的一个变化, 值得单独说:
 *     裸机时代"灯在闪"等于"主循环还活着", 因为翻转就发生在那个循环里。
 *     进了 RTOS 之后, LedTask 是独立跑的 —— 即使 UiTask 卡死, 这个任务
 *     照样每 500ms 翻一次灯, 那个故障指示就废了。
 *
 *   所以这里显式地把判断做出来: UiTask 每轮把 s_ui_alive 加一, LedTask
 *   发现它超过 UI_ALIVE_TIMEOUT_MS 没涨, 就转成 100ms 的快闪报警 ——
 *   和正常的 500ms 慢闪一眼就能区分开。
 * ====================================================================== */
static void LedTask(void *arg)
{
    uint32_t last_alive  = 0U;
    uint32_t last_change = 0U;

    (void)arg;

    for (;;)
    {
        if (s_ui_alive != last_alive)
        {
            last_alive  = s_ui_alive;
            last_change = Tick_GetMs();
            Led_Heartbeat();                    /* 正常: 每 500ms 翻一次 */
        }
        else if (Tick_Elapsed(last_change, UI_ALIVE_TIMEOUT_MS) != 0U)
        {
            /* UiTask 卡住了 -> 快闪(100ms 周期), 和正常心跳区分开 */
            Led_Force((uint8_t)((Tick_GetMs() / 100U) & 1U));
        }
        else
        {
            Led_Heartbeat();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ========================================================================
 * UiTask 的一轮 —— 串口回显 + 取输入 + 按界面干活
 *
 * ⚠ 摄像头画面**不在这里画**了: 它跟着抓帧一起挪到了 CameraTask。
 *   这个函数现在只剩菜单和舵机界面, 所以跑得很快 —— 这正是按键能立刻
 *   响应的原因。
 * ====================================================================== */
static void App_UiRun(void)
{
    uint8_t ch;

    /* ---- 1. 串口回显(不阻塞, 有多少发多少) ---- */
    while (Usart_ReadByte(&ch))
    {
        Usart_SendBytes(&ch, 1U);
    }

#if (UI_LVGL != 0)
    /* ================= LVGL 路径 =================
       ⚠ 这里**一行 Encoder_xxx 都没有**, 是故意的:
         编码器归 LVGL 的输入设备独占(原因见 UI_Enter 里那段)。
         输入由 Lvgl_TaskHandler() 里的输入定时器(33ms)消费,
         事件再经 Ui_Screens.c 的回调 -> App_ActionCb 回到这里。 */

    /* ---- 2. 处理排队的界面动作 ----
       ⚠ 必须在这里做, 不能在当时就做: 那些动作是在 LVGL 的事件派发**内部**
         被登记的, 那时切屏等于把正在处理事件的界面拆掉。详见 App_ActionCb。 */
    if (s_pending_act != 0xFFU)
    {
        ui_action_t act = (ui_action_t)s_pending_act;

        s_pending_act = 0xFFU;      /* 先清掉, 免得下面的 UI_Enter 又触发一次 */

        switch (act)
        {
            case UI_ACT_ENTER_CAMERA: UI_Enter(UI_CAMERA);    break;
            case UI_ACT_ENTER_SERVO:  UI_Enter(UI_SERVO);     break;
            case UI_ACT_BACK_MAIN:    UI_Enter(UI_MAIN_MENU); break;
            default: break;
        }
        /* 不用在这里额外调 Lvgl_TaskHandler —— UiTask 的循环紧接着就会调,
           新界面在同一次循环里就画出来了, 没有可见的空窗。 */
    }

    /* ---- 3. 摄像头: 有新帧就渲染一帧 ---- */
    UiCam_SetFps(s_fps);            /* 值没变时它内部直接返回, 不动 LVGL */

    if (s_screen == UI_CAMERA)
    {
        /* ⚠ 这个界面判断是**双保险**:
             UiCam_Pump 只认"帧序号变了没", 而 CameraTask 只在摄像头界面时
             才推进序号 —— 所以理论上这里不判也不会画错。
             但显式判一下更清楚, 也挡掉"刚切走、还有一格序号没消费"的边界。 */
        UiCam_Pump();
    }

#else   /* ================= 手写界面那条路 ================= */
    uint8_t pressed;
    int32_t delta;

    /* ---- 2. 输入 ----
       按键: 事件式, "取走即清"(采样在 FreeRTOS 的 1ms tick 钩子里,
             见 Tick.c 的 vApplicationTickHook)
       旋转: 读净格数并清零, 内部用 taskENTER_CRITICAL 保护 */
    pressed = Encoder_SwTakePress();
    delta   = Encoder_ReadDelta();

#if (UI_DIAG != 0)
    Diag_PrintLevels();     /* 电平巡检, 变了才打 */
    if (pressed != 0U) { Usart_SendBytes((const uint8_t *)"[SW]\r\n", 6U); }
    if (delta != 0)
    {
        char    db[12];
        uint8_t dn  = 0U;
        int32_t mag = (delta < 0) ? -delta : delta;

        db[dn++] = (uint8_t)'[';
        db[dn++] = (uint8_t)'R';
        db[dn++] = (uint8_t)'O';
        db[dn++] = (uint8_t)'T';
        db[dn++] = (uint8_t)' ';
        db[dn++] = (uint8_t)((delta < 0) ? '-' : '+');
        if (mag >= 10) { db[dn++] = (uint8_t)('0' + ((mag / 10) % 10)); }
        db[dn++] = (uint8_t)('0' + (mag % 10));
        db[dn++] = (uint8_t)']';
        db[dn++] = (uint8_t)'\r';
        db[dn++] = (uint8_t)'\n';

        Usart_SendBytes((const uint8_t *)db, dn);
    }
#endif

    /* ---- 3. 按当前界面干活 ---- */
    switch (s_screen)
    {
        case UI_SERVO:     UI_Servo(delta, pressed);    break;
        case UI_CAMERA:    UI_Camera(pressed);          break;
        case UI_MAIN_MENU:
        default:           UI_MainMenu(delta, pressed); break;
    }
#endif /* UI_LVGL */
}

/* App.c 不再对外暴露 App_Run —— 现在整个循环体归 UiTask 私有。 */

/* ========================================================================
 * 界面动作回调 —— Ui_Screens.c 在用户操作时调到这里
 *
 *   ⚠⚠ 这个函数是在 **LVGL 的事件派发内部**被调的。调用链:
 *
 *     UiTask -> Lvgl_TaskHandler -> lv_timer_handler -> 输入定时器
 *            -> indev 处理编码器 -> 控件的 lv_event 派发
 *            -> Ui_Screens.c 里的 Ui_xxxCb -> 这里
 *
 *   所以它**绝不能**调用 lv_screen_load() / lv_obj_delete() 这类会改动
 *   界面树的东西 —— 那等于"处理某个控件的事件处理到一半, 把它所在的整个
 *   界面换掉", 而 LVGL 手里还拿着旧界面的对象指针往下走。
 *
 *   正确做法: 只**登记意图**, 真正的动作留给 App_UiRun() 在派发之外执行。
 *   代价是最多一个轮询周期(5ms)的延迟 —— 手感上完全感觉不到。
 * ====================================================================== */
static void App_ActionCb(ui_action_t act, uint8_t arg)
{
    /* ---- 舵机角度: 例外, 立刻做 ----
       ⚠ 为什么不排队: 界面上的滑条和数字在回调里**已经**更新了(那是在改
         控件自己的属性, 在事件派发内部做是安全的), 如果硬件动作排队等 5ms,
         会出现"条已经动了、舵机还没动"的错位感。
       ⚠ 为什么在这里调是安全的: Servo_SetAngle 只写 PWM 比较寄存器,
         完全不碰 LVGL 的任何状态 —— 排队规则管的是"别拆 LVGL 的界面树",
         不是"回调里什么都不能做"。 */
    if (act == UI_ACT_SERVO_ANGLE)
    {
        s_angle = arg;
        Servo_SetAngle(s_angle);
        return;
    }

    /* ---- 换界面: 排队, 由 App_UiRun 处理 ---- */
    s_pending_act = (uint8_t)act;
}

/* ======================= 界面切换 ======================= */
/* 切到某个界面。三步顺序不能反。 */
static void UI_Enter(ui_screen_t s)
{
    /* ① 先改状态, **再**画。
       顺序有讲究: s_screen 一改, CameraTask 就不会再推进帧序号了 ——
       反过来的话会出现"菜单刚切好又被相机帧盖掉"的窗口。 */
    s_screen = s;

#if (UI_LVGL != 0)
    /* ---- LVGL 路径 ---- */

    /* ⚠ 这里**不再冲 Encoder_ReadDelta()**, 这一行是这次改造最容易漏的地方。
       原因: 编码器现在归 LVGL 的输入设备**独占** —— 它的输入定时器每 33ms
       就来"读走并清零"一次。我们这边再读一次的话, 那个"读走即清"的语义
       会把格数劈成两半, 现象是**转动丢步**(走一格有时生效有时不生效)。

       ⚠ 但 Encoder_SwTakePress() 照旧要吞: 它是**边沿锁存**, 和 LVGL 用的
         Encoder_SwIsDown()(电平)是两条独立的路径, 互不影响。
         不吞的话那个锁存会一直攒着, 以后有人用它时拿到一个陈年旧事件。 */
    (void)Encoder_SwTakePress();

    switch (s)
    {
        case UI_SERVO:
            UiScreens_Enter(UI_SCR_SERVO);
            /* 恢复上一次的角度。⚠ 显示和真值都要设:
               UiScreens_SetAngle 只改界面(它是渲染缓存),
               Servo_SetAngle 才是真的动舵机。 */
            UiScreens_SetAngle(s_angle);
            Servo_SetAngle(s_angle);
            break;

        case UI_CAMERA:
            /* 摄像头屏的控件在 UiCam_Build() 里就建好了, 这里只切显示。
               "无信号"标签的显隐是 Build 时按 buf 是否为 NULL 决定的。 */
            UiScreens_Enter(UI_SCR_CAMERA);
            s_frames = 0U;
            s_fps    = 0U;
            s_fps_t0 = Tick_GetMs();
            break;

        case UI_MAIN_MENU:
        default:
            UiScreens_Enter(UI_SCR_MAIN);
            break;
    }
#else
    /* ---- 手写界面那条路(UI_LVGL = 0 时) ---- */

    /* ① 冲掉上一屏残留的旋转格数。
          不清的话: 在主菜单转到"舵机"项按下进入, 那几格残留会在进入的
          瞬间被当成舵机界面的输入, **舵机自己就转过去了**。 */
    (void)Encoder_ReadDelta();

    /* ② 吞掉"造成这次切换"的那次按下。
          不清的话: 它还在锁存里, 下一轮又把它取出来处理一次 ——
          现象是一进功能界面就立刻弹回主菜单, **看着像菜单根本进不去**。 */
    (void)Encoder_SwTakePress();

    /* ③ 画该界面的静态部分(各 Draw*Chrome 内部自带加锁, 会清屏) */
    switch (s)
    {
        case UI_SERVO:
            Menu_DrawServoChrome();
            Servo_SetAngle(s_angle);        /* 回到这个界面时恢复上次的角度 */
            Menu_DrawServoValue(s_angle);
            break;

        case UI_CAMERA:
            Menu_DrawCameraChrome();
            s_frames = 0U;
            s_fps    = 0U;
            s_fps_t0 = Tick_GetMs();
            if (OV7670_IsPresent() == 0U)
            {
                Menu_DrawCameraNoSignal();  /* 没接就不进抓帧路径了, 一直显示它 */
            }
            break;

        case UI_MAIN_MENU:
        default:
            Menu_DrawMain(s_sel);
            break;
    }
#endif /* UI_LVGL */

    /* 叫醒 CameraTask, 让它重新判断该抓帧还是该睡。
       ⚠ 放最后: 先把该切的切完再通知。 */
    if (s_cam_task != NULL)
    {
        (void)xTaskNotifyGive(s_cam_task);
    }
}

#if (UI_LVGL == 0)
/* ======================= 主菜单 ======================= */
static void UI_MainMenu(int32_t delta, uint8_t pressed)
{
    if (pressed != 0U)
    {
        UI_Enter((s_sel == (uint8_t)MENU_ITEM_CAMERA) ? UI_CAMERA : UI_SERVO);
        return;
    }

    if (delta != 0)
    {
        uint8_t old = s_sel;
        int32_t n   = (int32_t)s_sel + delta;

        /* 两端环绕: 在第一项再往上转就到末项 */
        while (n < 0)                          { n += (int32_t)MENU_ITEM_COUNT; }
        n %= (int32_t)MENU_ITEM_COUNT;
        s_sel = (uint8_t)n;

        /* 只重画箭头和方框, 不整屏重画 —— LCD_Clear 是 15.6ms 的 DMA,
           每转一格来一次的话菜单会明显发滞 */
        if (s_sel != old)
        {
            Menu_DrawCursor(old, 0U);
            Menu_DrawCursor(s_sel, 1U);
        }
    }
}

/* ======================= 舵机界面 ======================= */
static void UI_Servo(int32_t delta, uint8_t pressed)
{
    int32_t a;

    if (pressed != 0U)                          /* 再按一次 = 退出 */
    {
        UI_Enter(UI_MAIN_MENU);
        return;
    }

    if (delta == 0) { return; }

    a = (int32_t)s_angle + delta * (int32_t)SERVO_STEP_DEG;

    /* 撞到行程端点就停在端点, 不绕回另一头 */
    if (a < 0)   { a = 0; }
    if (a > 180) { a = 180; }

    if ((uint8_t)a != s_angle)                  /* 已经在端点还往同方向转就别刷屏 */
    {
        s_angle = (uint8_t)a;
        Servo_SetAngle(s_angle);
        Menu_DrawServoValue(s_angle);
    }
}

/* ======================= 摄像头界面 ======================= */
/* 摄像头界面在 UiTask 这边只剩一件事: 响应"再按一次退出"。
   抓帧和送屏全部挪到了 CameraTask —— 这正是按键能立刻响应的原因:
   UiTask 再也不需要等那 66.7ms 的一帧了。

   ⚠ 别把抓帧搬回这里。搬回来就等于退回移植前的行为。 */
static void UI_Camera(uint8_t pressed)
{
    if (pressed != 0U)                          /* 再按一次 = 退出 */
    {
        UI_Enter(UI_MAIN_MENU);
    }
}

#endif /* UI_LVGL == 0 —— 手写界面的三个函数到此为止 */

/* ======================= 开机横幅 ======================= */
/* 打一行摄像头自检结果。
   **这是接好线之后第一个该看的东西**:
     CAM OK   -> SCCB 通了(接线/供电/上拉都没问题), 后面出问题就都在 FIFO 那边
     CAM FAIL -> 先去查 SIO_C(PE15)/SIO_D(PB0) 这两根线

   末尾的 VS=n/10 是 VSYNC 极性诊断, 判读方法见 OV7670.h 的
   OV7670_VsyncIdleHigh()。摄像头调通之后这一整段可以删掉。

   ⚠ 这段是"跨领域"的: 它同时用 camera(读寄存器)和 comms(打印)。正因为
     如此才放在 App 层 —— 否则 camera/ 就得 include Usart.h, 摄像头模块
     就不再自包含了。 */
static void App_SendCamId(void)
{
    static const char HEXD[] = "0123456789ABCDEF";
    static const char OK[]   = "CAM OK   PID=0x";
    static const char BAD[]  = "CAM FAIL PID=0x";
    uint8_t  pid = OV7670_ReadReg(0x0A);
    uint8_t  ver = OV7670_ReadReg(0x0B);
    const char *p = (pid == 0x76U) ? OK : BAD;
    uint8_t  buf[40];                       /* 最长约 33 字节, 留足余量 */
    uint8_t  n = 0U;
    uint8_t  vs;

    while (*p != '\0') { buf[n++] = (uint8_t)(*p++); }

    buf[n++] = (uint8_t)HEXD[(pid >> 4) & 0x0FU];
    buf[n++] = (uint8_t)HEXD[pid & 0x0FU];
    buf[n++] = (uint8_t)' ';
    buf[n++] = (uint8_t)'V';
    buf[n++] = (uint8_t)'R';
    buf[n++] = (uint8_t)'=';
    buf[n++] = (uint8_t)'0';
    buf[n++] = (uint8_t)'x';
    buf[n++] = (uint8_t)HEXD[(ver >> 4) & 0x0FU];
    buf[n++] = (uint8_t)HEXD[ver & 0x0FU];

    /* VSYNC 极性诊断(约 250ms)。
       ⚠ 只在**摄像头确实在线**时才做: 摄像头没接的话 VSYNC 恒低, 采出来必然是
         VS=0/0, 什么信息都没有, 白白拖慢开机四分之一秒。
         而摄像头在线时它是决定性的 —— 见 OV7670.h 的 OV7670_VsyncIdleHigh()。 */
    buf[n++] = (uint8_t)' ';
    buf[n++] = (uint8_t)'V';
    buf[n++] = (uint8_t)'S';
    buf[n++] = (uint8_t)'=';
    if (pid == 0x76U)
    {
        vs = OV7670_VsyncIdleHigh();
        buf[n++] = (uint8_t)('0' + (vs / 10U));
        buf[n++] = (uint8_t)'/';
        buf[n++] = (uint8_t)('0' + (vs % 10U));
    }
    else
    {
        buf[n++] = (uint8_t)'-';        /* 摄像头不在线, 不测 */
        buf[n++] = (uint8_t)'-';
    }

    buf[n++] = (uint8_t)'\r';
    buf[n++] = (uint8_t)'\n';

    Usart_SendBytes(buf, n);
}

#if (UI_DIAG != 0)
/* ======================= 电平巡检(临时诊断) ======================= */
/* A/B/SW 三根线只要有一根电平变了就立刻打一行, 形如  A=1 B=0 SW=1

   怎么用(排查编码器接线最有用的一个东西):
     正常转动编码器时应当看到 A 和 B 交替在 0/1 之间变化, 而 SW **全程保持 1**
       (EC11 停在定位点上时 A、B 通常都是 1; 转到两格之间才会有一个变 0)
     转动时看到 SW 变成 0        -> 假按键就是从这来的
     按键时看到 A 或 B 变成 0    -> 按键动作在干扰 A/B
     转一整圈 A/B 从来没到过 0   -> **编码器的公共脚没有真正接地**
                                     (A/B 只有内部 40k 上拉, 没回路拉不低,
                                      只能靠容性耦合出毛刺 —— 现象就是"能计到数
                                      但两个方向互相串")。2026-09-22 就是这个。 */
static uint8_t s_diag_lv = 0xFFU;

static void Diag_PrintLevels(void)
{
    uint8_t a = (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_A_PIN)  != Bit_RESET) ? 1U : 0U;
    uint8_t b = (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_B_PIN)  != Bit_RESET) ? 1U : 0U;
    uint8_t s = (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_SW_PIN) != Bit_RESET) ? 1U : 0U;
    uint8_t lv = (uint8_t)((a << 2) | (b << 1) | s);
    uint8_t buf[20];
    uint8_t n = 0U;

    if (lv == s_diag_lv) { return; }
    s_diag_lv = lv;

    buf[n++] = (uint8_t)'A'; buf[n++] = (uint8_t)'='; buf[n++] = (uint8_t)('0' + a);
    buf[n++] = (uint8_t)' ';
    buf[n++] = (uint8_t)'B'; buf[n++] = (uint8_t)'='; buf[n++] = (uint8_t)('0' + b);
    buf[n++] = (uint8_t)' ';
    buf[n++] = (uint8_t)'S'; buf[n++] = (uint8_t)'W'; buf[n++] = (uint8_t)'=';
    buf[n++] = (uint8_t)('0' + s);
    buf[n++] = (uint8_t)'\r'; buf[n++] = (uint8_t)'\n';

    Usart_SendBytes(buf, n);
}
#endif

/* ========================================================================
 * FreeRTOS 钩子 —— 教学脚手架, 开关在 FreeRTOSConfig.h
 *
 * 为什么值得开:
 *   初学 RTOS 最常见的翻车是"系统莫名卡死" —— 任务栈给小了、或者
 *   configTOTAL_HEAP_SIZE 不够导致建任务失败。**默认情况下这两种都不报错**,
 *   只是行为诡异(某个任务再也不跑 / 某个任务根本没建起来)。
 *   开了这两个钩子, 出问题会当场停在明处。
 *
 * ⚠ 钩子里只做最简单的活: 打印 + 点灯 + 死循环。**不要调任何 FreeRTOS API,
 *   不要 malloc** —— 走到这里内核状态已经不可信了。
 * ⚠ 字符串长度一律用 sizeof(字面量) - 1 算, 不手写数字, 免得数错发出去乱码。
 * ====================================================================== */

/* 任务栈溢出(由 configCHECK_FOR_STACK_OVERFLOW = 2 触发)。
   pcTaskName 是任务名, 能直接告诉你是谁溢出。 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    static const char P1[] = "\r\n[RTOS] 栈溢出! 任务 = ";
    static const char P2[] = "\r\n";

    (void)xTask;

    Usart_SendBytes((const uint8_t *)P1, sizeof(P1) - 1U);

    if (pcTaskName != NULL)
    {
        /* 任务名是 '\0' 结尾的短字符串, 逐字符发 */
        while (*pcTaskName != '\0')
        {
            Usart_SendBytes((const uint8_t *)pcTaskName, 1U);
            pcTaskName++;
        }
    }

    Usart_SendBytes((const uint8_t *)P2, sizeof(P2) - 1U);

    Led_Force(1U);              /* 灯常亮 = 出事了(正常是每 500ms 翻一次) */
    for (;;)
    {
    }
}

/* 内核堆耗尽(建任务/队列时 pvPortMalloc 失败)。
   最常见的修法是把 FreeRTOSConfig.h 里的 configTOTAL_HEAP_SIZE 调大,
   或者把某个任务的栈调小。 */
void vApplicationMallocFailedHook(void)
{
    static const char MSG[] =
        "\r\n[RTOS] 堆耗尽! 把 configTOTAL_HEAP_SIZE 调大, 或减小任务栈\r\n";

    Usart_SendBytes((const uint8_t *)MSG, sizeof(MSG) - 1U);

    Led_Force(1U);
    for (;;)
    {
    }
}
