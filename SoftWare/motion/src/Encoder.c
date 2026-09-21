/**
  ******************************************************************************
  * @file    SoftWare/src/Encoder.c
  * @brief   旋转编码器中断驱动实现(判向逻辑详见 Encoder.h)
  *
  *          工作流程:
  *
  *            [A 相下降沿 -> EXTI6 中断]
  *                 | 读 B 相: B=低 -> CCW++; B=高 -> CW++
  *                 | 屏蔽 EXTI(该格内不再响应)
  *                 | 启动 TIM7 3ms 单次计数
  *                 v
  *            [TIM7 中断(3ms 到)]
  *                 停 TIM7 -> 放开 EXTI -> 允许计下一格
  *
  *          好处: 抖动沿发生在“EXTI 屏蔽 + TIM 倒计时”窗口内被忽略;
  *                主循环不管多忙都不影响计次; 判定在边沿瞬间完成, 方向最准
  ******************************************************************************
  */

#include "Encoder.h"
#include "Tick.h"       /* 按键采样要挂到 1ms 时基上(见 Encoder_Init 末尾) */

#include "FreeRTOS.h"
#include "task.h"

/* 按键采样函数: 只给 Tick 的 1ms 回调用, 不对外暴露 */
static void Encoder_SwTick1ms(void);

/* ---------------- 内部状态(ISR 与 main 共享, 加 volatile) ---------------- */
static volatile uint32_t s_cw   = 0U;  /* 正转次数 */
static volatile uint32_t s_ccw  = 0U;  /* 反转次数 */
static volatile uint8_t  s_busy = 0U;  /* 1 = 一次触发后的消抖锁定窗口 */

/* SW 按键(在 1ms 中断里采样, 主循环只取事件) */
static volatile uint8_t  s_sw_stable = 1U;  /* 消抖后的电平: 1=松开 0=按下 */
static volatile uint8_t  s_sw_cnt    = 0U;  /* 与稳定电平不一致的连续毫秒数 */
static volatile uint8_t  s_sw_press  = 0U;  /* 按下事件标志, 主循环取走即清 */

/* ======================= NVIC 抢占优先级 =======================
   ⚠ FreeRTOS 的硬性规定: 任何要调用 ...FromISR() 的中断, 抢占优先级必须
     **数值上 ≥ configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY**(本工程是 5)。
     数值越小优先级越高, 所以原来的 4 属于"比阈值还高" —— 那种中断不会被
     内核的临界区屏蔽, 也就绝不能在里面调 FreeRTOS API, 否则会命中
     vPortValidateInterruptPriority() 的断言。

     EXTI 原来是 4, 现在统一成 5。这两个中断目前确实没调 FreeRTOS API
     (只改几个 volatile 变量、读写寄存器), 但设成合规的值以后想加就随时能加。

     ⚠ 光改这里没用 —— 还必须先在 main() 里
       NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4),
       否则 StdPeriph 算出来的寄存器值会把这些数字**整体丢掉**。
       (移植前就是这个状态: 三个中断实际同优先级, 写的 4 和 5 一个字节都没生效) */
#define ENC_EXTI_PRIO   5U
#define ENC_TIM_PRIO    5U

/* 临界区: 直接转发到 FreeRTOS 的宏。
 *
 * 2026-09-22 移植 FreeRTOS 前这里是 __disable_irq() / __set_PRIMASK(),
 * 也就是**关全局中断**(PRIMASK) —— 那会把内核的 tick 和整个调度器一起挡掉。
 * FreeRTOS 的 taskENTER_CRITICAL()/taskEXIT_CRITICAL() 用的是 BASEPRI,
 * 只屏蔽"优先级低于阈值"的中断, tick 和 PendSV 照常走, 这才是内核期望的用法。
 *
 * ⚠ 这三个函数的调用者全都在**任务上下文**(UiTask/AppTask), 所以用任务版
 *   的宏是对的。中断里要用得换 portSET_INTERRUPT_MASK_FROM_ISR()。
 * ⚠ 临界区里不许调用任何会阻塞的 API(包括 vTaskDelay / 带 FromISR 的),
 *   本文件里只是几条读改写, 没问题。 */
static uint32_t Enc_CriticalEnter(void)
{
  taskENTER_CRITICAL();
  return 0U;                    /* 返回值保留着, 免得改一堆调用点 */
}

static void Enc_CriticalExit(uint32_t primask)
{
  (void)primask;                /* taskEXIT_CRITICAL 内部自己维护嵌套计数 */
  taskEXIT_CRITICAL();
}

/* ========================================================================
 * 初始化: GPIO(输入上拉) + EXTI6(下降沿) + TIM7(3ms) + NVIC
 * ====================================================================== */
void Encoder_Init(void)
{
  GPIO_InitTypeDef         GPIO_InitStructure;
  EXTI_InitTypeDef         EXTI_InitStructure;
  NVIC_InitTypeDef         NVIC_InitStructure;
  TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;

  /* ---- GPIO: A/B 相 + SW 按键都配成上拉输入(空闲高) ---- */
  RCC_AHB1PeriphClockCmd(ENC_GPIO_CLK, ENABLE);

  GPIO_StructInit(&GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin  = ENC_A_PIN | ENC_B_PIN | ENC_SW_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(ENC_GPIO_PORT, &GPIO_InitStructure);

  /* ---- EXTI: A 相(PB6) 下降沿触发 ---- */
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
  SYSCFG_EXTILineConfig(ENC_EXTI_PORT_SRC, ENC_EXTI_PIN_SRC);

  EXTI_InitStructure.EXTI_Line    = ENC_EXTI_LINE;
  EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
  EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling; /* A: 高 -> 低 */
  EXTI_InitStructure.EXTI_LineCmd = ENABLE;
  EXTI_Init(&EXTI_InitStructure);

  NVIC_InitStructure.NVIC_IRQChannel                   = ENC_EXTI_IRQn;
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = ENC_EXTI_PRIO;
  NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
  NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
  NVIC_Init(&NVIC_InitStructure);

  /* ---- TIM7: 3ms 消抖窗口(计数到后触发更新中断) ---- */
  RCC_APB1PeriphClockCmd(ENC_TIM_CLK, ENABLE);

  TIM_TimeBaseStructure.TIM_Prescaler         = ENC_TIM_PSC;  /* 84MHz/84=1MHz */
  TIM_TimeBaseStructure.TIM_CounterMode       = TIM_CounterMode_Up;
  TIM_TimeBaseStructure.TIM_Period            = ENC_TIM_ARR;  /* 2999 -> 3ms */
  TIM_TimeBaseStructure.TIM_ClockDivision     = TIM_CKD_DIV1;
  TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
  TIM_TimeBaseInit(TIM7, &TIM_TimeBaseStructure);

  TIM_Cmd(TIM7, DISABLE);                     /* 先关, 用到再开 */
  TIM_ITConfig(TIM7, TIM_IT_Update, DISABLE);

  NVIC_InitStructure.NVIC_IRQChannel                   = TIM7_IRQn;
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = ENC_TIM_PRIO;
  NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
  NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
  NVIC_Init(&NVIC_InitStructure);

  /* ---- 复位状态 ---- */
  EXTI_ClearITPendingBit(ENC_EXTI_LINE);
  s_cw   = 0U;
  s_ccw  = 0U;
  s_busy = 0U;

  s_sw_cnt    = 0U;
  s_sw_press  = 0U;
  /* 按当前实际电平初始化, 别默认"松开" —— 否则上电时如果按钮正好被按住,
     要等松开再按才会产生第一次事件 */
  s_sw_stable = (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_SW_PIN) == Bit_RESET) ? 0U : 1U;

  /* ---- 把按键采样挂到 1ms 时基上 ----
     这么做而不是让 Tick 直接调 Encoder, 是为了**依赖方向**:
     motion 依赖 system(上层用下层)是对的; 反过来让 system 去 include
     motion/Encoder.h 就成了底层依赖上层, system/ 也不再自包含。

     采样必须在 1ms 中断里做, 不能放主循环: 摄像头界面一轮约 95ms,
     轮询会漏掉快速点按; 而且判电平的话, 一次按下会连续多轮都算"按下",
     导致进功能界面后立刻弹回主菜单。 */
  Tick_SetMsCallback(Encoder_SwTick1ms);
}

/* ========================================================================
 * EXTI9_5 中断(实际只用了线 6): A 相下降沿 -> 判向 + 计次
 * 注意: EXTI 线 5~9 共用此中断函数; 若以后还用了其它 5~9 线,
 *       需要在这里一起判断对应的 EXTI_GetITStatus。
 * ====================================================================== */
void EXTI9_5_IRQHandler(void)
{
  if (EXTI_GetITStatus(ENC_EXTI_LINE) != RESET)
  {
    EXTI_ClearITPendingBit(ENC_EXTI_LINE);

    if (s_busy)
    {
      return;                 /* 上一次触发的消抖窗口还没结束, 忽略 */
    }
    s_busy = 1U;

    /* A 刚由高变低: 此刻读 B, 判定方向 */
    if (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_B_PIN) == Bit_RESET)
    {
      s_ccw++;                /* B=低 -> 反转 */
    }
    else
    {
      s_cw++;                 /* B=高 -> 正转 */
    }

    /* 屏蔽 EXTI, 启动 TIM7: 窗口内抖动沿一律不响应 */
    EXTI->IMR &= (uint32_t)~ENC_EXTI_LINE;

    TIM7->CNT = 0U;
    TIM_Cmd(TIM7, ENABLE);
    TIM_ITConfig(TIM7, TIM_IT_Update, ENABLE);
  }
}

/* ========================================================================
 * TIM7 中断: 3ms 消抖窗口结束 -> 放开 EXTI, 允许计下一格
 * ====================================================================== */
void TIM7_IRQHandler(void)
{
  if (TIM_GetITStatus(TIM7, TIM_IT_Update) != RESET)
  {
    TIM_ClearITPendingBit(TIM7, TIM_IT_Update);

    TIM_Cmd(TIM7, DISABLE);
    TIM_ITConfig(TIM7, TIM_IT_Update, DISABLE);

    /* ⚠ 顺序要紧: 必须先把 s_busy 清掉, 再放开 EXTI。
       EXTI(抢占优先级 4) 能抢占 TIM7(优先级 5) —— 若先放开 EXTI 才清 s_busy,
       恰好插在这两条之间来的那个 A 相下降沿, 会在 EXTI ISR 里看到 s_busy
       还是 1 而提前 return, 而且它的 pending 位已经被清掉了 —— **那一格被
       静默丢弃**, 不报任何错。先撤锁再开闸门就没有这个窗口。 */
    s_busy = 0U;
    EXTI_ClearITPendingBit(ENC_EXTI_LINE);   /* 清掉窗口期间可能残留的 pending */
    EXTI->IMR |= ENC_EXTI_LINE;
  }
}

/* ========================================================================
 * 旋转: 读"净转格数"并清零(应用层主力接口)
 *
 *   读 + 清零必须在同一个临界区里完成。分两步做的话, 中间被 EXTI 插进来
 *   多计一格, 那一格就被清零吃掉了 —— 表现为偶尔"转了没反应"。
 *
 *   返回带符号: 正转正数、反转负数。内部用无符号相减再转有符号,
 *   即使计数器溢出回绕, 差值依然是对的。
 * ====================================================================== */
int32_t Encoder_ReadDelta(void)
{
  int32_t  delta;
  uint32_t pm = Enc_CriticalEnter();

  delta = (int32_t)(s_cw - s_ccw);
  s_cw  = 0U;
  s_ccw = 0U;

  Enc_CriticalExit(pm);
  return delta;
}

/* ========================================================================
 * SW 按键: 1ms 采样 + 消抖 + "按下边沿"锁存
 *
 *   只在电平由高变低(按下)的那一次置标志, 松开不置。
 *   配合 Encoder_SwTakePress() 的"取走即清", 一次按下在结构上只可能被
 *   主循环处理一次 —— 这是"进功能界面立刻弹回主菜单"那个 bug 的根治办法。
 *
 *   ⚠ 必须由 1ms 中断调用。放主循环里轮询会漏掉快速点按:
 *     摄像头界面一轮约 95ms, 一次 30ms 的点按在两次采样之间就过去了。
 * ====================================================================== */
static void Encoder_SwTick1ms(void)
{
  /* SW 是上拉输入: 松开 = 高, 按下 = 低 */
  uint8_t level = (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_SW_PIN) == Bit_RESET) ? 0U : 1U;

  if (level == s_sw_stable)
  {
    s_sw_cnt = 0U;                     /* 电平没变, 计数清零 */
    return;
  }

  if (++s_sw_cnt < ENC_SW_DEBOUNCE_MS)
  {
    return;                            /* 抖动还没稳够, 继续攒 */
  }

  s_sw_cnt    = 0U;
  s_sw_stable = level;

  if (level == 0U)                     /* 确认按下 */
  {
    s_sw_press = 1U;
  }
}

uint8_t Encoder_SwTakePress(void)
{
  uint8_t  press;
  uint32_t pm = Enc_CriticalEnter();

  press      = s_sw_press;
  s_sw_press = 0U;

  Enc_CriticalExit(pm);
  return press;
}

/* ========================================================================
 * 底层计数(调试/特殊场合用; 常规应用请用 Encoder_ReadDelta)
 * ====================================================================== */
uint32_t Encoder_GetCW(void)
{
  return s_cw;
}

uint32_t Encoder_GetCCW(void)
{
  return s_ccw;
}

void Encoder_ResetCount(void)
{
  uint32_t pm = Enc_CriticalEnter();

  s_cw  = 0U;
  s_ccw = 0U;

  Enc_CriticalExit(pm);
}
