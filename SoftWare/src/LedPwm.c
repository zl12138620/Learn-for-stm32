/**
  ******************************************************************************
  * @file    SoftWare/src/LedPwm.c
  * @brief   用户 LED(LED2 / PB2) 软件 PWM 调光实现
  *
  *          原理: PB2 没有任何定时器复用(详见 LedPwm.h 的说明), 做不了硬件
  *                PWM, 所以拿 TIM4 的更新中断当"心跳": 每来一次中断就把亮度
  *                计数器加一, 与目标亮度比较后决定 PB2 输出高还是低;
  *                走满 LEDPWM_STEPS 级就是一整个 PWM 周期。
  *
  *          注意: 板载 LED2 是**高电平点亮**(阳极接 PB2, 阴极经 R29 到 GND),
  *                所以占空比越大越亮。
  ******************************************************************************
  */

#include "LedPwm.h"

/* ======================= 引脚 / 定时器配置 ======================= */
/* PB2 是本板 LED 的硬件走线, 换脚等于要飞线, 正常不用改 */
#define LEDPWM_GPIO_CLK     RCC_AHB1Periph_GPIOB
#define LEDPWM_GPIO_PORT    GPIOB
#define LEDPWM_GPIO_PIN     GPIO_Pin_2 | GPIO_Pin_1

#define LEDPWM_TIM_CLK      RCC_APB1Periph_TIM4
#define LEDPWM_TIM          TIM4

/* APB1 定时器时钟(与 SoftWarePWM.c / Servo.c / SoftUSART.c 保持一致) */
#define LEDPWM_TIMER_HZ     84000000UL

/* 计数频率 2MHz -> 0.5us 一级 */
#define LEDPWM_TICK_HZ      2000000UL

/* 亮度级数 = 一个 PWM 周期里的中断次数。级数越多调光越细腻, 中断也越密;
   100 级对应 20kHz 中断 / 200Hz PWM, 是细腻度和 CPU 开销的折中 */
#define LEDPWM_STEPS        100U

#define LEDPWM_DUTY_DEFAULT 50U     /* 默认半亮 */

/* ======================= 内部状态(ISR 与 main 共享, 加 volatile) ======================= */
static volatile uint8_t s_duty = LEDPWM_DUTY_DEFAULT;   /* 目标亮度 0~100 */
static volatile uint8_t s_step = 0U;                    /* 当前走到第几级 */

/* ========================================================================
 * 初始化: PB2 推挽输出 + TIM4 时基 + 更新中断 + NVIC
 * ====================================================================== */
void LedPwm_Init(void)
{
  GPIO_InitTypeDef        gpio;
  TIM_TimeBaseInitTypeDef timebase;
  NVIC_InitTypeDef        nvic;

  RCC_AHB1PeriphClockCmd(LEDPWM_GPIO_CLK, ENABLE);
  RCC_APB1PeriphClockCmd(LEDPWM_TIM_CLK,  ENABLE);

  /* ---- PB2: 推挽输出, 由中断翻转(不经过定时器的输出通道) ---- */
  GPIO_StructInit(&gpio);
  gpio.GPIO_Pin   = LEDPWM_GPIO_PIN;
  gpio.GPIO_Mode  = GPIO_Mode_OUT;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
  gpio.GPIO_Speed = GPIO_Speed_100MHz;
  GPIO_Init(LEDPWM_GPIO_PORT, &gpio);
  GPIO_ResetBits(LEDPWM_GPIO_PORT, LEDPWM_GPIO_PIN);   /* 先灭, 等中断把亮度做起来 */

  /* ---- TIM4 时基: 84MHz / 42 = 2MHz, 走 100 个计数触发一次更新中断 ---- */
  TIM_TimeBaseStructInit(&timebase);
  timebase.TIM_Prescaler         = (uint16_t)((LEDPWM_TIMER_HZ / LEDPWM_TICK_HZ) - 1U);
  timebase.TIM_CounterMode       = TIM_CounterMode_Up;
  timebase.TIM_Period            = (uint16_t)(LEDPWM_STEPS - 1U);
  timebase.TIM_ClockDivision     = TIM_CKD_DIV1;
  timebase.TIM_RepetitionCounter = 0;
  TIM_TimeBaseInit(LEDPWM_TIM, &timebase);

  TIM_ClearITPendingBit(LEDPWM_TIM, TIM_IT_Update);
  TIM_ITConfig(LEDPWM_TIM, TIM_IT_Update, ENABLE);

  /* 优先级放到最低: 调光不怕抖动, 不该去抢占串口/编码器的中断 */
  nvic.NVIC_IRQChannel                   = TIM4_IRQn;
  nvic.NVIC_IRQChannelPreemptionPriority = 6;
  nvic.NVIC_IRQChannelSubPriority        = 0;
  nvic.NVIC_IRQChannelCmd                = ENABLE;
  NVIC_Init(&nvic);

  TIM_Cmd(LEDPWM_TIM, ENABLE);
}

/* ========================================================================
 * TIM4 更新中断: 一级一级地做 PWM
 *
 *   每进一次中断就是 PWM 周期里的一"级":
 *     s_step <  s_duty -> 点亮
 *     s_step >= s_duty -> 熄灭
 *   走满 LEDPWM_STEPS 级后回到 0 重新开始。
 *   比如 s_duty = 50, 则前 50 级亮、后 50 级灭 -> 50% 亮度。
 * ====================================================================== */
void TIM4_IRQHandler(void)
{
  if (TIM_GetITStatus(LEDPWM_TIM, TIM_IT_Update) != RESET)
  {
    TIM_ClearITPendingBit(LEDPWM_TIM, TIM_IT_Update);

    if (s_step < s_duty)
    {
      GPIO_SetBits(LEDPWM_GPIO_PORT, LEDPWM_GPIO_PIN);      /* 高电平点亮 */
    }
    else
    {
      GPIO_ResetBits(LEDPWM_GPIO_PORT, LEDPWM_GPIO_PIN);
    }

    s_step++;
    if (s_step >= LEDPWM_STEPS)
    {
      s_step = 0U;                                          /* 一个 PWM 周期结束 */
    }
  }
}

/* ========================================================================
 * 对外接口
 * ====================================================================== */
void LedPwm_SetDuty(uint8_t percent)
{
  if (percent > 100U)
  {
    percent = 100U;      /* 超出范围夹紧 */
  }
  s_duty = percent;
}

uint8_t LedPwm_GetDuty(void)
{
  return s_duty;
}
