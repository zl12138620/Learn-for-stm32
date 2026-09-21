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
#include "Tick.h"

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

static ui_screen_t s_screen = UI_MAIN_MENU;
static uint8_t     s_sel    = 0U;       /* 主菜单当前选中项 */
static uint8_t     s_angle  = 90U;      /* 舵机当前角度(上电回中位) */

/* 帧率统计: 数 1 秒窗口内抓到几帧 */
static uint32_t s_frames = 0U;
static uint32_t s_fps_t0 = 0U;
static uint8_t  s_fps    = 0U;

/* ======================= 函数声明 ======================= */
static void UI_Enter(ui_screen_t s);
static void UI_MainMenu(int32_t delta, uint8_t pressed);
static void UI_Servo(int32_t delta, uint8_t pressed);
static void UI_Camera(uint8_t pressed);
static void App_SendCamId(void);

#if (UI_DIAG != 0)
static void Diag_PrintLevels(void);
#endif

/* ========================================================================
 * 开机
 * ====================================================================== */
void App_Init(void)
{
    App_SendCamId();                /* 横幅: 摄像头自检结果, 接好线后第一个该看的 */
    UI_Enter(UI_MAIN_MENU);
}

/* ========================================================================
 * 主循环体
 * ====================================================================== */
void App_Run(void)
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
       按键: 事件式, "取走即清"(采样在 1ms 中断里, 见 Encoder_SwTick1ms)
       旋转: 读净格数并清零, 内部关中断保护 */
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

    s_screen = s;

    /* ③ 画该界面的静态部分(各 Draw*Chrome 内部会清屏) */
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
static void UI_Camera(uint8_t pressed)
{
    if (pressed != 0U)                          /* 再按一次 = 退出 */
    {
        UI_Enter(UI_MAIN_MENU);
        return;
    }

    /* 开机就探测到摄像头不在线: 画面停在"无信号"不动, 不要去抓帧 ——
       否则每轮要白等 3 次 200ms 超时, 按键会变得几乎没反应 */
    if (OV7670_IsPresent() == 0U) { return; }

    /* 抓一帧。抓不到就**跳过读出** —— 读出来的是 FIFO 里的陈旧内容(上一帧
       的残影), 显示它还不如保持上一帧不动 */
    if (OV7670_CaptureFrame() == 0U) { return; }

    OV7670_ReadFrameRotated();
    LCD_DrawImage(4U, 0U, CAM_ROT_W, CAM_ROT_H, OV7670_GetFrameBuf());

    /* ---- 帧率: 数满 1 秒算一次 ----
       摄像头只在这里抓帧, 所以这个数就是真实的显示帧率。
       实测约 8~9fps, 别以为是 bug: 等相机出一整帧就要 66.7ms(12MHz 晶振),
       再叠加读 FIFO 并旋转 ~35ms、DMA 送屏 14.6ms。 */
    s_frames++;
    if (Tick_Elapsed(s_fps_t0, 1000U) != 0U)
    {
        s_fps    = (s_frames > 99U) ? 99U : (uint8_t)s_frames;
        s_frames = 0U;
        s_fps_t0 = Tick_GetMs();
    }

    /* ⚠ 每帧都要重画, 而且必须在 LCD_DrawImage() 之后 —— 画面会把右上角盖掉 */
    Menu_DrawCameraFps(s_fps);
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
