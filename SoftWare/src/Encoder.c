/**
  ******************************************************************************
  * @file    SoftWare/src/Encoder.c
  * @brief   旋转编码器中断驱动实现(判向逻辑详见 Encoder.h)
  *
  *          工作流程:
  *
  *            [A 相下降沿 -> EXTI8 中断]
  *                 | 读 B 相: B=低 -> CCW++; B=高 -> CW++
  *                 | 屏蔽 EXTI8(该格内不再响应)
  *                 | 启动 TIM7 3ms 单次计数
  *                 v
  *            [TIM7 中断(3ms 到)]
  *                 停 TIM7 -> 放开 EXTI8 -> 允许计下一格
  *
  *          好处: 抖动沿发生在“EXTI 屏蔽 + TIM 倒计时”窗口内被忽略;
  *                主循环不管多忙都不影响计次; 判定在边沿瞬间完成, 方向最准
  ******************************************************************************
  */

#include "Encoder.h"

/* ---------------- 内部状态(ISR 与 main 共享, 加 volatile) ---------------- */
static volatile uint32_t s_cw   = 0U;  /* 正转次数 */
static volatile uint32_t s_ccw  = 0U;  /* 反转次数 */
static volatile uint8_t  s_busy = 0U;  /* 1 = 一次触发后的消抖锁定窗口 */

/* ========================================================================
 * 初始化: GPIO(输入上拉) + EXTI8(下降沿) + TIM7(3ms) + NVIC
 * ====================================================================== */
void Encoder_Init(void)
{
  GPIO_InitTypeDef         GPIO_InitStructure;
  EXTI_InitTypeDef         EXTI_InitStructure;
  NVIC_InitTypeDef         NVIC_InitStructure;
  TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;

  /* ---- GPIO: PB7/PB8 输入 + 上拉(空闲高) ---- */
  RCC_AHB1PeriphClockCmd(ENC_GPIO_CLK, ENABLE);

  GPIO_StructInit(&GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin  = ENC_A_PIN | ENC_B_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(ENC_GPIO_PORT, &GPIO_InitStructure);

  /* ---- EXTI: A 相(PB8) 下降沿触发 ---- */
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
  SYSCFG_EXTILineConfig(ENC_EXTI_PORT_SRC, ENC_EXTI_PIN_SRC);

  EXTI_InitStructure.EXTI_Line    = ENC_EXTI_LINE;
  EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
  EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling; /* A: 高 -> 低 */
  EXTI_InitStructure.EXTI_LineCmd = ENABLE;
  EXTI_Init(&EXTI_InitStructure);

  NVIC_InitStructure.NVIC_IRQChannel                   = ENC_EXTI_IRQn;
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 4;
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
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;
  NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
  NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
  NVIC_Init(&NVIC_InitStructure);

  /* ---- 复位状态 ---- */
  EXTI_ClearITPendingBit(ENC_EXTI_LINE);
  s_cw   = 0U;
  s_ccw  = 0U;
  s_busy = 0U;
}

/* ========================================================================
 * EXTI9_5 中断(实际只用了线 8): A 相下降沿 -> 判向 + 计次
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

    /* 先清可能残留的 pending, 再放开 EXTI */
    EXTI_ClearITPendingBit(ENC_EXTI_LINE);
    EXTI->IMR |= ENC_EXTI_LINE;
    s_busy = 0U;
  }
}

/* ========================================================================
 * 对外查询接口(main 随时调用)
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
  __disable_irq();      /* 运行中清零需与中断互斥, 临时关中断 */
  s_cw  = 0U;
  s_ccw = 0U;
  __enable_irq();
}
