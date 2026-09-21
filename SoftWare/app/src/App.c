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
#define UI_TASK_STACK       512U
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

/* UiTask 超过这么久没动静就认定它卡住了, LedTask 转成快闪报警 */
#define UI_ALIVE_TIMEOUT_MS 1000U

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

static uint8_t     s_sel    = 0U;       /* 主菜单当前选中项 —— 只有 UiTask 碰 */
static uint8_t     s_angle  = 90U;      /* 舵机当前角度(上电回中位) —— 只有 UiTask 碰 */

/* 帧率统计: 数 1 秒窗口内抓到几帧 —— 只有 CameraTask 碰 */
static uint32_t s_frames = 0U;
static uint32_t s_fps_t0 = 0U;
static uint8_t  s_fps    = 0U;

/* ======================= 函数声明 ======================= */
static void UI_Enter(ui_screen_t s);
static void UI_MainMenu(int32_t delta, uint8_t pressed);
static void UI_Servo(int32_t delta, uint8_t pressed);
static void UI_Camera(uint8_t pressed);
static void App_UiRun(void);
static void App_SendCamId(void);

#if (UI_DIAG != 0)
static void Diag_PrintLevels(void);
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

    /* 建失败会调到 vApplicationMallocFailedHook(堆不够), 不会静默过去 */
    (void)xTaskCreate(UiTask,     "Ui",  UI_TASK_STACK,  NULL, UI_TASK_PRIO,  NULL);
    (void)xTaskCreate(CameraTask, "Cam", CAM_TASK_STACK, NULL, CAM_TASK_PRIO, &s_cam_task);
    (void)xTaskCreate(LedTask,    "Led", LED_TASK_STACK, NULL, LED_TASK_PRIO, NULL);
}

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
    UI_Enter(UI_MAIN_MENU);

    for (;;)
    {
        s_ui_alive++;               /* 给 LedTask 看的存活心跳(见 LedTask 注释) */

        App_UiRun();

        /* ⚠ 必须让出 CPU: 这个任务 prio 4, 空转会饿死 CameraTask(2) 和
           LedTask(1)。5ms 的轮询周期对按键手感毫无影响 ——
           按键本身是 1ms tick 钩子在采样并锁存的, 不会漏。
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

        OV7670_ReadFrameRotated();

        /* ⚠ 抓帧这 95ms 里用户可能已经退出摄像头界面了 —— 那就**不要画**,
           否则会把相机画面盖到刚画好的菜单上, 而且没人会再重画菜单。
           (这只是第一道闸; 真正保证正确性的是下面把判断放进锁里,
            见 Menu_DrawCameraFrameLocked 的注释。) */
        if (s_screen != UI_CAMERA) { continue; }

        /* ---- 帧率: 数满 1 秒算一次 ---- */
        s_frames++;
        if (Tick_Elapsed(s_fps_t0, 1000U) != 0U)
        {
            s_fps    = (s_frames > 99U) ? 99U : (uint8_t)s_frames;
            s_frames = 0U;
            s_fps_t0 = Tick_GetMs();
        }

        /* ⚠ 判断和画**必须在同一个锁里** —— 锁外判断的话, 存在"判断完还在
           摄像头界面、等拿到锁时已经切走了"的窗口, 那帧就会盖到菜单上。
           详见 Menu.c 里这个函数的注释。 */
        Menu_Lock();
        if (s_screen == UI_CAMERA)
        {
            Menu_DrawCameraFrameLocked(OV7670_GetFrameBuf(), s_fps);
        }
        Menu_Unlock();
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
    uint8_t pressed;
    int32_t delta;

    /* ---- 1. 串口回显(不阻塞, 有多少发多少) ---- */
    while (Usart_ReadByte(&ch))
    {
        Usart_SendBytes(&ch, 1U);
    }

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
}

/* App.c 不再对外暴露 App_Run —— 现在整个循环体归 UiTask 私有。 */

/* ======================= 界面切换 ======================= */
/* 切到某个界面。三步顺序不能反。 */
static void UI_Enter(ui_screen_t s)
{
    /* ① 冲掉上一屏残留的旋转格数。
          不清的话: 在主菜单转到"舵机"项按下进入, 那几格残留会在进入的
          瞬间被当成舵机界面的输入, **舵机自己就转过去了**。 */
    (void)Encoder_ReadDelta();

    /* ② 吞掉"造成这次切换"的那次按下。
          不清的话: 它还在锁存里, 下一轮又把它取出来处理一次 ——
          现象是一进功能界面就立刻弹回主菜单, **看着像菜单根本进不去**。 */
    (void)Encoder_SwTakePress();

    /* ③ 先改状态, **再**画。
       顺序有讲究: s_screen 一改, CameraTask 就算醒着也不会再画了
       (它在锁内会复查这个值)。反过来的话, 会出现"菜单刚画好又被相机帧
       盖掉"的窗口。 */
    s_screen = s;

    /* ④ 画该界面的静态部分(各 Draw*Chrome 内部自带加锁, 会清屏) */
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

    /* ⑤ 叫醒 CameraTask, 让它重新判断该抓帧还是该睡。
       ⚠ 放最后: 先把该画的画完再通知, 免得它抢在前面把画面画出来,
         又被这里的 Chrome 覆盖掉(白画一帧, 还要多占一次锁)。 */
    if (s_cam_task != NULL)
    {
        (void)xTaskNotifyGive(s_cam_task);
    }
}

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
