/**
  ******************************************************************************
  * @file    SoftWare/src/Servo.c
  * @brief   SG90 舵机驱动实现 —— PA1 / TIM5_CH2
  *
  *          时基: APB1 定时器时钟 84MHz, 预分频 84 -> 计数频率 1MHz
  *                (1 个计数 = 1us, 20ms 周期 -> ARR = 19999)
  *          输出: PWM1 模式, 高电平有效; ARR/CCR 都开预装载, 运行中改角度
  *                不会产生毛刺, 新值在下一个更新事件生效
  *
  *          注意: 内核 168MHz 时 APB1 = 42MHz, 定时器再 x2 = 84MHz, 才有
  *                SERVO_TIMER_HZ; 若改系统时钟请同步修改该宏。
  ******************************************************************************
  */

#include "Servo.h"

/* ======================= 引脚 / 定时器配置 ======================= */
/* 改引脚时下面 5 项要一起改, 并注意本模块不用中断, 无需改 NVIC/EXTI */
#define SERVO_GPIO_CLK      RCC_AHB1Periph_GPIOA
#define SERVO_GPIO_PORT     GPIOA
#define SERVO_GPIO_PIN      GPIO_Pin_1
#define SERVO_GPIO_PIN_SRC  GPIO_PinSource1
#define SERVO_GPIO_AF       GPIO_AF_TIM5

#define SERVO_TIM_CLK       RCC_APB1Periph_TIM5
#define SERVO_TIM           TIM5

/* APB1 定时器时钟(与 SoftWarePWM.c / SoftUSART.c 保持一致) */
#define SERVO_TIMER_HZ      84000000UL

/* 计数频率: 1MHz -> 分辨率 1us */
#define SERVO_TICK_HZ       1000000UL

/* 舵机周期 20ms = 50Hz */
#define SERVO_PERIOD_US     20000U

/* 脉宽行程: SG90 标称 0.5ms~2.5ms 对应 0~180°。
   若两端有嗡嗡声/抖动, 说明这颗舵机的机械行程更窄, 收窄成 1000/2000 再试。 */
#define SERVO_PULSE_MIN_US  500U
#define SERVO_PULSE_MAX_US  2500U

#define SERVO_ANGLE_MAX     180U
#define SERVO_ANGLE_DEFAULT 90U
#define SERVO_PULSE_DEFAULT 1500U

/* ======================= 内部状态 ======================= */
static uint8_t  s_angle = SERVO_ANGLE_DEFAULT;   /* 当前角度 */
static uint16_t s_pulse = SERVO_PULSE_DEFAULT;   /* 当前脉宽(us) */

/* ======================= 换算 ======================= */
/* 脉宽(us) -> CCR 比较值。
   计数频率 1MHz 时 1 个计数就是 1us, 乘除会被编译器折掉; 保留通式是为了
   万一以后改 SERVO_TICK_HZ, 这里不用跟着动。 */
static uint32_t Servo_CcrFromPulse(uint16_t us)
{
  return ((uint32_t)us * SERVO_TICK_HZ) / 1000000UL;
}

/* 角度 -> 脉宽, 并同步刷新 CCR。
   先乘后除: 先算 (span * deg) 再除以 180, 避免中间结果被截断掉精度。 */
static void Servo_ApplyAngle(uint8_t deg)
{
  uint32_t span = (uint32_t)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US);

  if (deg > (uint8_t)SERVO_ANGLE_MAX)
  {
    deg = (uint8_t)SERVO_ANGLE_MAX;      /* 超出范围夹紧, 不报错 */
  }

  s_angle = deg;
  s_pulse = (uint16_t)(SERVO_PULSE_MIN_US + (span * (uint32_t)deg) / SERVO_ANGLE_MAX);

  SERVO_TIM->CCR2 = Servo_CcrFromPulse(s_pulse);
}

/* 脉宽 -> 角度, 并同步刷新 CCR。
   脉宽被夹紧过之后反算回角度, 这样 Servo_GetAngle() 不会和实际输出对不上。 */
static void Servo_ApplyPulse(uint16_t us)
{
  uint32_t span = (uint32_t)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US);

  if (us < SERVO_PULSE_MIN_US) { us = SERVO_PULSE_MIN_US; }
  if (us > SERVO_PULSE_MAX_US) { us = SERVO_PULSE_MAX_US; }

  s_pulse = us;
  s_angle = (uint8_t)(((uint32_t)(us - SERVO_PULSE_MIN_US) * SERVO_ANGLE_MAX) / span);

  SERVO_TIM->CCR2 = Servo_CcrFromPulse(s_pulse);
}

/* ======================= 引脚形态切换 ======================= */
/* 切到 TIM5_CH2 复用功能 */
static void Servo_PinToAF(void)
{
  GPIO_InitTypeDef gpio;

  GPIO_PinAFConfig(SERVO_GPIO_PORT, SERVO_GPIO_PIN_SRC, SERVO_GPIO_AF);

  GPIO_StructInit(&gpio);
  gpio.GPIO_Pin   = SERVO_GPIO_PIN;
  gpio.GPIO_Mode  = GPIO_Mode_AF;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(SERVO_GPIO_PORT, &gpio);
}

/* 关掉复用后引脚会悬空, 所以主动切成推挽输出并拉低 ——
   舵机收到的是一根持续低电平, 既不会抖也不会乱转。 */
static void Servo_PinToLowOutput(void)
{
  GPIO_InitTypeDef gpio;

  GPIO_StructInit(&gpio);
  gpio.GPIO_Pin   = SERVO_GPIO_PIN;
  gpio.GPIO_Mode  = GPIO_Mode_OUT;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(SERVO_GPIO_PORT, &gpio);

  GPIO_ResetBits(SERVO_GPIO_PORT, SERVO_GPIO_PIN);
}

/* ======================= 初始化 ======================= */
void Servo_Init(void)
{
  TIM_TimeBaseInitTypeDef timebase;
  TIM_OCInitTypeDef       oc;

  RCC_AHB1PeriphClockCmd(SERVO_GPIO_CLK, ENABLE);
  RCC_APB1PeriphClockCmd(SERVO_TIM_CLK,  ENABLE);

  Servo_PinToAF();                      /* PA1 -> AF2(TIM5_CH2) */

  /* ---- 时基: 84MHz / 84 = 1MHz, 周期 20ms ---- */
  TIM_TimeBaseStructInit(&timebase);
  timebase.TIM_Prescaler         = (uint16_t)((SERVO_TIMER_HZ / SERVO_TICK_HZ) - 1U);
  timebase.TIM_CounterMode       = TIM_CounterMode_Up;
  timebase.TIM_Period            = (uint16_t)(SERVO_PERIOD_US - 1U);
  timebase.TIM_ClockDivision     = TIM_CKD_DIV1;
  timebase.TIM_RepetitionCounter = 0;
  TIM_TimeBaseInit(SERVO_TIM, &timebase);

  /* ---- 通道 2: PWM1 模式, 高电平有效 ---- */
  TIM_OCStructInit(&oc);
  oc.TIM_OCMode      = TIM_OCMode_PWM1;
  oc.TIM_OutputState = TIM_OutputState_Enable;
  oc.TIM_OCPolarity  = TIM_OCPolarity_High;
  oc.TIM_Pulse       = Servo_CcrFromPulse(SERVO_PULSE_DEFAULT);
  TIM_OC2Init(SERVO_TIM, &oc);

  /* 预装载: 运行中改 CCR 不会产生毛刺 */
  TIM_OC2PreloadConfig(SERVO_TIM, TIM_OCPreload_Enable);
  TIM_ARRPreloadConfig(SERVO_TIM, ENABLE);

  TIM_Cmd(SERVO_TIM, ENABLE);

  Servo_ApplyAngle((uint8_t)SERVO_ANGLE_DEFAULT);   /* 上电回中位 */
}

void Servo_Stop(void)
{
  TIM_Cmd(SERVO_TIM, DISABLE);
  Servo_PinToLowOutput();
}

/* ======================= 参数设置 / 读取 ======================= */
void Servo_SetAngle(uint8_t deg)
{
  Servo_ApplyAngle(deg);
}

uint8_t Servo_GetAngle(void)
{
  return s_angle;
}

void Servo_SetPulseUs(uint16_t us)
{
  Servo_ApplyPulse(us);
}

uint16_t Servo_GetPulseUs(void)
{
  return s_pulse;
}
