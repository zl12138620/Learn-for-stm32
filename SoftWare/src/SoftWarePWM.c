/**
  ******************************************************************************
  * @file    SoftWare/src/SoftWarePWM.c
  * @brief   硬件 PWM 输出实现 —— PB4 / TIM3_CH1
  *
  *          时基: APB1 定时器时钟 84MHz, 预分频 84 -> 计数频率 1MHz
  *                (1 个计数 = 1us, 16bit ARR 可覆盖 16Hz ~ 1MHz)
  *          输出: PWM1 模式, 高电平有效; ARR/CCR 都开预装载, 运行中改参数
  *                不会产生毛刺, 新值在下一个更新事件生效
  *
  *          注意: 内核 168MHz 时 APB1 = 42MHz, 定时器再 x2 = 84MHz, 才有
  *                PWM_TIMER_HZ; 若改系统时钟请同步修改该宏。
  ******************************************************************************
  */

#include "SoftWarePWM.h"

/* ======================= 引脚 / 定时器配置 ======================= */
/* 改引脚时下面 5 项要一起改, 并注意 EXTI/中断编号无需变动(本模块不用中断) */
#define PWM_GPIO_CLK      RCC_AHB1Periph_GPIOB
#define PWM_GPIO_PORT     GPIOB
#define PWM_GPIO_PIN      GPIO_Pin_4
#define PWM_GPIO_PIN_SRC  GPIO_PinSource4
#define PWM_GPIO_AF       GPIO_AF_TIM3

#define PWM_TIM_CLK       RCC_APB1Periph_TIM3
#define PWM_TIM           TIM3

/* APB1 定时器时钟(与 SoftUSART.c 的 SOFTUSART_TIMER_HZ 保持一致) */
#define PWM_TIMER_HZ      84000000UL

/* 计数频率: 1MHz -> 分辨率 1us */
#define PWM_TICK_HZ       1000000UL

#define PWM_ARR_MAX       0xFFFFU     /* 16bit 自动重装寄存器上限 */

#define PWM_FREQ_DEFAULT  1000U       /* 默认 1kHz */
#define PWM_DUTY_DEFAULT  50U         /* 默认 50% */

/* ======================= 内部状态 ======================= */
static uint32_t s_freq = PWM_FREQ_DEFAULT;
static uint8_t  s_duty = PWM_DUTY_DEFAULT;

/* ======================= 换算 ======================= */
/* 频率 -> ARR(自动重装值)。频率取 0 按 1Hz 处理; 结果夹到 16bit 合法范围 */
static uint32_t PWM_ArrFromFreq(uint32_t hz)
{
  uint32_t ticks;                       /* 一个周期多少个计数 */

  if (hz == 0U)
  {
    hz = 1U;
  }

  ticks = PWM_TICK_HZ / hz;             /* 周期计数 = 计数频率 / 目标频率 */
  if (ticks == 0U)
  {
    ticks = 1U;                         /* 目标频率高于计数频率, 取最快 */
  }
  if (ticks > (PWM_ARR_MAX + 1U))
  {
    ticks = PWM_ARR_MAX + 1U;           /* 目标频率过低, 取最慢 */
  }

  return ticks - 1U;                    /* ARR = 周期计数 - 1 */
}

/* 占空比(0~100) -> CCR(比较值) */
static uint32_t PWM_CcrFromDuty(uint32_t arr, uint8_t percent)
{
  if (percent > 100U)
  {
    percent = 100U;
  }
  return ((arr + 1U) * (uint32_t)percent) / 100U;
}

/* ======================= 引脚形态切换 ======================= */
/* 切到 TIM3_CH1 复用功能 */
static void PWM_PinToAF(void)
{
  GPIO_InitTypeDef gpio;

  GPIO_PinAFConfig(PWM_GPIO_PORT, PWM_GPIO_PIN_SRC, PWM_GPIO_AF);

  GPIO_StructInit(&gpio);
  gpio.GPIO_Pin   = PWM_GPIO_PIN;
  gpio.GPIO_Mode  = GPIO_Mode_AF;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(PWM_GPIO_PORT, &gpio);
}

/* 关掉复用后引脚会悬空, 所以主动切成推挽输出并拉低, 保证蜂鸣器不响 */
static void PWM_PinToLowOutput(void)
{
  GPIO_InitTypeDef gpio;

  GPIO_StructInit(&gpio);
  gpio.GPIO_Pin   = PWM_GPIO_PIN;
  gpio.GPIO_Mode  = GPIO_Mode_OUT;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(PWM_GPIO_PORT, &gpio);

  GPIO_ResetBits(PWM_GPIO_PORT, PWM_GPIO_PIN);
}

/* ======================= 初始化 ======================= */
void PWM_Init(void)
{
  TIM_TimeBaseInitTypeDef timebase;
  TIM_OCInitTypeDef       oc;

  RCC_AHB1PeriphClockCmd(PWM_GPIO_CLK, ENABLE);
  RCC_APB1PeriphClockCmd(PWM_TIM_CLK,  ENABLE);

  PWM_PinToAF();                        /* PB4 -> AF2(TIM3_CH1) */

  /* ---- 时基: 84MHz / 84 = 1MHz ---- */
  TIM_TimeBaseStructInit(&timebase);
  timebase.TIM_Prescaler         = (uint16_t)((PWM_TIMER_HZ / PWM_TICK_HZ) - 1U);
  timebase.TIM_CounterMode       = TIM_CounterMode_Up;
  timebase.TIM_Period            = (uint16_t)PWM_ArrFromFreq(s_freq);
  timebase.TIM_ClockDivision     = TIM_CKD_DIV1;
  timebase.TIM_RepetitionCounter = 0;
  TIM_TimeBaseInit(PWM_TIM, &timebase);

  /* ---- 通道 1: PWM1 模式, 高电平有效 ---- */
  TIM_OCStructInit(&oc);
  oc.TIM_OCMode      = TIM_OCMode_PWM1;
  oc.TIM_OutputState = TIM_OutputState_Enable;
  oc.TIM_OCPolarity  = TIM_OCPolarity_High;
  oc.TIM_Pulse       = (uint16_t)PWM_CcrFromDuty(timebase.TIM_Period, s_duty);
  TIM_OC1Init(PWM_TIM, &oc);

  /* 预装载: 运行中改 ARR/CCR 不会产生毛刺 */
  TIM_OC1PreloadConfig(PWM_TIM, TIM_OCPreload_Enable);
  TIM_ARRPreloadConfig(PWM_TIM, ENABLE);

  TIM_Cmd(PWM_TIM, ENABLE);
}

/* ======================= 启停 ======================= */
void PWM_Start(void)
{
  PWM_PinToAF();
  TIM_Cmd(PWM_TIM, ENABLE);
}

void PWM_Stop(void)
{
  TIM_Cmd(PWM_TIM, DISABLE);
  PWM_PinToLowOutput();
}

/* ======================= 参数设置 ======================= */
void PWM_SetFreq(uint32_t hz)
{
  uint32_t arr;

  s_freq = hz;
  arr    = PWM_ArrFromFreq(hz);

  PWM_TIM->ARR = arr;
  /* ARR 变了, 同一个占空比对应的比较值也要跟着重算 */
  PWM_TIM->CCR1 = PWM_CcrFromDuty(arr, s_duty);
}

void PWM_SetDuty(uint8_t percent)
{
  if (percent > 100U)
  {
    percent = 100U;
  }
  s_duty = percent;

  PWM_TIM->CCR1 = PWM_CcrFromDuty(PWM_TIM->ARR, percent);
}

/* ======================= 读取 ======================= */
uint32_t PWM_GetFreq(void)
{
  return s_freq;
}

uint8_t PWM_GetDuty(void)
{
  return s_duty;
}
