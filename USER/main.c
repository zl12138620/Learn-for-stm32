/**
  ******************************************************************************
  * @file    USER/main.c
  * @brief   用户按键(SW2/WKUP/PA0) 控制 用户LED(LED2/PB2) 亮灭
  *          板子  : 立创梁山派·天空星 (LCKFB-YZH V1.0, 2024-01-17)
  *          芯片  : STM32F407VGT6 (与原理图引脚兼容)
  *          原理图 : 资料/立创梁山派·天空星开发板原理图_2024-01-17.pdf
  *                    - 第2页: SW2 用户(唤醒)按键 -> 网络 WKUP; LED2 用户LED -> 网络 PB2(BOOT1)
  *                    - 第5页: PA0-WKUP(PA0) => PA0;  PB2-BOOT1(PB2) => PB2
  *          电平   : PA0 经10k上拉到3V3, 按键按下接地 => 平时高, 按下低
  *                    LED2 高边接 +3V3(经R29 2k), 低边到 PB2 => PB2=0 点亮
  ******************************************************************************
  */

#include "main.h"

/* ---------------- 用户按键: WKUP = PA0 ---------------- */
#define KEY_RCC_CLK   RCC_AHB1Periph_GPIOA
#define KEY_PORT      GPIOA
#define KEY_PIN       GPIO_Pin_0

/* ---------------- 用户 LED: BOOT1 = PB2 ---------------- */
#define LED_RCC_CLK   RCC_AHB1Periph_GPIOB
#define LED_PORT      GPIOB
#define LED_PIN       GPIO_Pin_2

/* ---------------- 有源蜂鸣器: BOOT1 = PB4 -------------- */
#define FMQ_RCC_CLK   RCC_AHB1Periph_GPIOB
#define FMQ_PORT      GPIOB
#define FMQ_PIN       GPIO_Pin_4

/* ---------------- 循迹模块: BOOT1 = PA7 -------------- */
#define FINDER_PORT     GPIOA
#define FINDER_PIN      GPIO_Pin_7
#define FINDER_RCC_CLK  RCC_AHB1Periph_GPIOA

static void Delay_ms(uint32_t ms);
static void USER_FINDER_CONFIG(void);
static void USER_FMQ_CONFIG(void);
static void USER_LED_CONFIG(void);




int main(void)
{
  USER_FINDER_CONFIG();
  USER_FMQ_CONFIG();
  USER_LED_CONFIG();
  // GPIO_WriteBit(LED_PORT, LED_PIN, Bit_SET);



  while (1)
  {
      if(GPIO_ReadInputDataBit(FINDER_PORT, FINDER_PIN) == Bit_RESET)
      {
        Delay_ms(20);//延时判断防止误判
        if(GPIO_ReadInputDataBit(FINDER_PORT, FINDER_PIN) == Bit_RESET)
        {
          GPIO_WriteBit(FMQ_PORT, FMQ_PIN, Bit_SET);
          GPIO_WriteBit(LED_PORT, LED_PIN, Bit_SET);
        }
          
      }
      else
      {
          GPIO_WriteBit(FMQ_PORT, FMQ_PIN, Bit_RESET);
          GPIO_WriteBit(LED_PORT, LED_PIN, Bit_RESET);

      }    
          // GPIO_WriteBit(LED_PORT, LED_PIN, Bit_RESET);
  }
}



/** 
 * @brief 用户LED
 * 
*/
static void USER_LED_CONFIG(void)
{
  RCC_AHB1PeriphClockCmd(LED_RCC_CLK, ENABLE);
  GPIO_InitTypeDef GPIO_StructInit;
  GPIO_StructInit.GPIO_Mode = GPIO_Mode_OUT;
  GPIO_StructInit.GPIO_OType = GPIO_OType_PP;
  GPIO_StructInit.GPIO_Pin = LED_PIN;
  GPIO_StructInit.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_StructInit.GPIO_Speed = GPIO_Speed_2MHz;
  GPIO_Init(LED_PORT, &GPIO_StructInit);
}



/**
  * @brief  循迹模块PA7
  */
static void USER_FINDER_CONFIG(void)
{
  RCC_AHB1PeriphClockCmd(FINDER_RCC_CLK, ENABLE);
  GPIO_InitTypeDef GPIO_StructInit;
  GPIO_StructInit.GPIO_Mode = GPIO_Mode_IN;
  GPIO_StructInit.GPIO_Pin = FINDER_PIN;
  GPIO_StructInit.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(FINDER_PORT, &GPIO_StructInit);
}


/**
  * @brief 蜂鸣器配置PB3
  */ 
static void USER_FMQ_CONFIG(void)
{
    RCC_AHB1PeriphClockCmd(FMQ_RCC_CLK, ENABLE);
    GPIO_InitTypeDef GPIO_StructInit;
    GPIO_StructInit.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_StructInit.GPIO_OType = GPIO_OType_PP;
    GPIO_StructInit.GPIO_Pin = FMQ_PIN;
    GPIO_StructInit.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_StructInit.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(FMQ_PORT, &GPIO_StructInit);
}

/**
  * @brief  轮询 SysTick 实现的毫秒延时(不依赖 SysTick 中断)
  * @param  ms: 延时毫秒数
  */
static void Delay_ms(uint32_t ms)
{
  uint32_t reload = SystemCoreClock / 1000U - 1U;   /* 168MHz 时约1ms */

  while (ms--)
  {
    SysTick->LOAD = reload;
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0)
    {
    }
  }
  SysTick->CTRL = 0;   /* 关闭 SysTick */
}


