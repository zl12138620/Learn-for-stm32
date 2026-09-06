/**
  ******************************************************************************
  * @file    SoftWare/src/SoftUSART.c
  * @brief   软件串口实现(bit-bang UART, 8N1, LSB first)
  *   - 发送: 复用 SysTick 作自由运行计数, 按内核时钟(168MHz)精确延时翻转 TX
  *   - 接收: RX 下降沿(起始位)进 EXTI 中断 -> 启动 TIM6 定时,
  *           在每 bit 中点采样; 首中断(0.5bit)校验 start 为低,
  *           之后每 1bit 采 D0..D7, 收满存入环形缓冲并重新使能 EXTI
  *   注意: 内核时钟需为 168MHz(SystemCoreClock), APB1 定时器时钟需 84MHz
  *         才与文件内常量匹配; 改系统时钟请同步修改宏 SOFTUSART_TIMER_HZ
  ******************************************************************************
  */

#include "SoftUSART.h"
#include "stm32f4xx_tim.h"

/* APB1 定时器时钟(默认 84MHz: APB1=42MHz, 定时器倍频x2) */
#define SOFTUSART_TIMER_HZ   84000000UL

/* ---------------- 发送/接收内部状态 ---------------- */
static uint32_t        s_txBitCycles;   /* 每 bit 对应的内核周期数(SysTick) */
static uint32_t        s_rxPerBit;      /* 每 bit 对应的 TIM6 计数 */
static uint32_t        s_rxHalf;        /* 0.5 bit 计数 */
static volatile uint8_t  s_rxBuf[SOFTUSART_RX_BUFSZ];
static volatile uint16_t s_rxHead = 0;  /* 写指针 */
static volatile uint16_t s_rxTail = 0;  /* 读指针 */
static volatile uint8_t  s_rxByte;      /* 正在组装的接收字节 */
static volatile uint8_t  s_rxCount;     /* 已采样位数(0 表示等待 start 校验) */
static volatile uint8_t  s_rxBusy;      /* 1 = 正在接收一帧 */

/* ========================================================================
 * 发送用 SysTick 自由运行计数(LOAD=0xFFFFFF, 计数内核时钟周期)
 * 说明: 使用期间会占用 SysTick, 不要与本函数嵌套使用其它 SysTick 延时
 * ====================================================================== */
static void TX_TickStart(void)
{
  SysTick->CTRL = 0;
  SysTick->LOAD = 0x00FFFFFFUL;         /* 24bit 最大值, 0.1s 才回绕一次 */
  SysTick->VAL  = 0;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

/* 等待 cyc 个内核时钟周期(cyc 需 < 2^24) */
static void TX_WaitCycles(uint32_t cyc)
{
  uint32_t start = SysTick->VAL;
  while (((start - SysTick->VAL) & 0x00FFFFFFUL) < cyc)
  {
  }
}

static void TX_TickStop(void)
{
  SysTick->CTRL = 0;
}

/* ========================================================================
 * 初始化: TX=PA2 推挽输出(空闲高), RX=PA3 上拉输入 + EXTI3 下降沿 + TIM6
 * ====================================================================== */
void SoftUSART_Init(void)
{
  GPIO_InitTypeDef     GPIO_InitStruct;
  EXTI_InitTypeDef     EXTI_InitStruct;
  NVIC_InitTypeDef     NVIC_InitStruct;
  TIM_TimeBaseInitTypeDef TIM_TimeBaseStruct;

  /* 位时间换算 */
  s_txBitCycles = SystemCoreClock / SOFTUSART_BAUD;
  s_rxPerBit    = SOFTUSART_TIMER_HZ / SOFTUSART_BAUD;
  s_rxHalf      = s_rxPerBit / 2U;

  /* ---- GPIO ---- */
  RCC_AHB1PeriphClockCmd(SOFTUSART_TX_RCC_CLK | SOFTUSART_RX_RCC_CLK, ENABLE);

  /* TX: 推挽输出, 空闲拉高 */
  GPIO_InitStruct.GPIO_Pin   = SOFTUSART_TX_PIN;
  GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_OUT;
  GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStruct.GPIO_PuPd  = GPIO_PuPd_NOPULL;
  GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(SOFTUSART_TX_PORT, &GPIO_InitStruct);
  GPIO_SetBits(SOFTUSART_TX_PORT, SOFTUSART_TX_PIN);   /* 空闲=高 */

  /* RX: 输入, 上拉(空闲高; 若对端推挽驱动也可用 NOPULL) */
  GPIO_InitStruct.GPIO_Pin   = SOFTUSART_RX_PIN;
  GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_IN;
  GPIO_InitStruct.GPIO_PuPd  = GPIO_PuPd_UP;
  GPIO_Init(SOFTUSART_RX_PORT, &GPIO_InitStruct);

  /* ---- EXTI: RX 下降沿(起始位) ---- */
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
  SYSCFG_EXTILineConfig(SOFTUSART_RX_PORT_SRC, SOFTUSART_RX_PIN_SRC);

  EXTI_InitStruct.EXTI_Line    = SOFTUSART_RX_EXTI_LINE;
  EXTI_InitStruct.EXTI_Mode    = EXTI_Mode_Interrupt;
  EXTI_InitStruct.EXTI_Trigger = EXTI_Trigger_Falling;
  EXTI_InitStruct.EXTI_LineCmd = ENABLE;
  EXTI_Init(&EXTI_InitStruct);

  NVIC_InitStruct.NVIC_IRQChannel                   = SOFTUSART_RX_EXTI_IRQn;
  NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 4;
  NVIC_InitStruct.NVIC_IRQChannelSubPriority        = 0;
  NVIC_InitStruct.NVIC_IRQChannelCmd                = ENABLE;
  NVIC_Init(&NVIC_InitStruct);

  /* ---- TIM6: 接收位定时(计数时钟 = 84MHz, 预分频 0) ---- */
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);

  TIM_TimeBaseStruct.TIM_Prescaler     = 0;
  TIM_TimeBaseStruct.TIM_CounterMode   = TIM_CounterMode_Up;
  TIM_TimeBaseStruct.TIM_Period        = 0xFFFF;
  TIM_TimeBaseStruct.TIM_ClockDivision = TIM_CKD_DIV1;
  TIM_TimeBaseStruct.TIM_RepetitionCounter = 0;
  TIM_TimeBaseInit(TIM6, &TIM_TimeBaseStruct);

  TIM_Cmd(TIM6, DISABLE);
  TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);

  NVIC_InitStruct.NVIC_IRQChannel                   = TIM6_DAC_IRQn;
  NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
  NVIC_InitStruct.NVIC_IRQChannelSubPriority        = 0;
  NVIC_InitStruct.NVIC_IRQChannelCmd                = ENABLE;
  NVIC_Init(&NVIC_InitStruct);
}

/* ========================================================================
 * 发送 1 字节: start(0) + D0..D7(LSB first) + stop(1)
 * ====================================================================== */
void SoftUSART_SendByte(uint8_t ch)
{
  uint8_t i;

  TX_TickStart();

  /* start bit: 拉低, 保持 1 位 */
  GPIO_WriteBit(SOFTUSART_TX_PORT, SOFTUSART_TX_PIN, Bit_RESET);
  TX_WaitCycles(s_txBitCycles);

  /* 8 个数据位, LSB first */
  for (i = 0; i < 8; i++)
  {
    if (ch & 0x01U)
    {
      GPIO_WriteBit(SOFTUSART_TX_PORT, SOFTUSART_TX_PIN, Bit_SET);
    }
    else
    {
      GPIO_WriteBit(SOFTUSART_TX_PORT, SOFTUSART_TX_PIN, Bit_RESET);
    }
    ch >>= 1;
    TX_WaitCycles(s_txBitCycles);
  }

  /* stop bit: 拉高, 保持 1 位 */
  GPIO_WriteBit(SOFTUSART_TX_PORT, SOFTUSART_TX_PIN, Bit_SET);
  TX_WaitCycles(s_txBitCycles);

  TX_TickStop();
}

void SoftUSART_SendString(const char *s)
{
  while (*s != '\0')
  {
    SoftUSART_SendByte((uint8_t)*s++);
  }
}

/* ========================================================================
 * 环形缓冲
 * ====================================================================== */
static void RX_Store(uint8_t ch)
{
  uint16_t next = (uint16_t)((s_rxHead + 1U) % SOFTUSART_RX_BUFSZ);

  if (next == s_rxTail)
  {
    return;                     /* 缓冲满: 丢弃该字节 */
  }
  s_rxBuf[s_rxHead] = ch;
  s_rxHead = next;
}

/* ========================================================================
 * 中止一帧接收并重新等待起始位
 * ====================================================================== */
static void RX_Abort(void)
{
  s_rxBusy  = 0;
  s_rxCount = 0;
  TIM_Cmd(TIM6, DISABLE);
  TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);
  EXTI_ClearITPendingBit(SOFTUSART_RX_EXTI_LINE);
  EXTI->IMR |= (uint32_t)SOFTUSART_RX_EXTI_LINE;   /* 重新检测起始位 */
}

/* ========================================================================
 * EXTI3: RX 下降沿 = 检测到起始位
 * ====================================================================== */
void EXTI3_IRQHandler(void)
{
  if (EXTI_GetITStatus(SOFTUSART_RX_EXTI_LINE) != RESET)
  {
    EXTI_ClearITPendingBit(SOFTUSART_RX_EXTI_LINE);

    if (s_rxBusy)
    {
      return;                   /* 正在接收, 忽略 */
    }

    /* 进入接收: 屏蔽 EXTI, 启动 TIM6, 0.5bit 后先校验 start bit */
    EXTI->IMR &= (uint32_t)~SOFTUSART_RX_EXTI_LINE;
    s_rxBusy  = 1;
    s_rxCount = 0;
    s_rxByte  = 0;

    TIM6->CNT = 0;
    TIM6->ARR = s_rxHalf - 1U;  /* 0.5bit 后产生第一次更新中断 */
    TIM_Cmd(TIM6, ENABLE);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);
  }
}

/* ========================================================================
 * TIM6: 每位中点采样 RX
 *   时序(相对下降沿): 0.5bit 校验 start=0 -> 1.5/2.5/.../8.5bit 采 D0..D7
 * ====================================================================== */
void TIM6_DAC_IRQHandler(void)
{
  uint8_t level;

  if (TIM_GetITStatus(TIM6, TIM_IT_Update) == RESET)
  {
    return;
  }
  TIM_ClearITPendingBit(TIM6, TIM_IT_Update);

  if (!s_rxBusy)
  {
    return;
  }

  level = (GPIO_ReadInputDataBit(SOFTUSART_RX_PORT, SOFTUSART_RX_PIN) != Bit_RESET)
          ? 1U : 0U;

  if (s_rxCount == 0U)
  {
    /* 起始位中心: 应为低; 若读到高说明是毛刺误触发, 放弃并重新等待 */
    if (level != 0U)
    {
      RX_Abort();
      return;
    }
    TIM6->ARR = s_rxPerBit - 1U;   /* 下一中断在 +1bit(D0 中心) */
    s_rxCount = 1U;
  }
  else
  {
    /* 采样数据位 D(count-1), LSB first */
    s_rxByte >>= 1U;
    if (level != 0U)
    {
      s_rxByte |= 0x80U;
    }

    if (s_rxCount >= 8U)
    {
      /* 一帧收完: 存缓冲并重新等待下一帧起始位 */
      RX_Store(s_rxByte);
      RX_Abort();
      return;
    }

    TIM6->ARR = s_rxPerBit - 1U;
    s_rxCount++;
  }
}

/* ========================================================================
 * 对外 API: 查询/读取接收数据
 * ====================================================================== */
uint8_t SoftUSART_RxReady(void)
{
  return (uint8_t)((s_rxHead - s_rxTail + SOFTUSART_RX_BUFSZ) % SOFTUSART_RX_BUFSZ);
}

uint8_t SoftUSART_RxByte(void)
{
  uint8_t ch;

  if (s_rxTail == s_rxHead)
  {
    return 0U;                   /* 无数据 */
  }
  ch = s_rxBuf[s_rxTail];
  s_rxTail = (uint16_t)((s_rxTail + 1U) % SOFTUSART_RX_BUFSZ);
  return ch;
}

