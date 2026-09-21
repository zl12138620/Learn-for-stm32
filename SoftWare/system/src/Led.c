/**
  ******************************************************************************
  * @file    SoftWare/src/Led.c
  * @brief   板载用户 LED(LED2 / PB2) 心跳指示实现 —— 详见 Led.h 的说明
  ******************************************************************************
  */

#include "Led.h"
#include "Tick.h"

/* ======================= 板载走线(改不了, 要换脚得飞线) ======================= */
#define LED_PORT        GPIOB
#define LED_PIN         GPIO_Pin_2

/* 上次翻转的时间戳 */
static uint32_t s_last_ms = 0U;

void Led_Init(void)
{
    GPIO_InitTypeDef g;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    GPIO_StructInit(&g);
    g.GPIO_Pin   = LED_PIN;
    g.GPIO_Mode  = GPIO_Mode_OUT;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    g.GPIO_Speed = GPIO_Speed_2MHz;        /* 心跳而已, 慢速就够, 少一点干扰 */
    GPIO_Init(LED_PORT, &g);

    GPIO_ResetBits(LED_PORT, LED_PIN);     /* 先灭 */
    s_last_ms = Tick_GetMs();
}

void Led_Heartbeat(void)
{
    if (Tick_Elapsed(s_last_ms, LED_HEARTBEAT_MS) == 0U) { return; }

    /* 用"上次的时间戳 + 周期"而不是"当前时间", 免得某一轮卡久了之后
       时间戳被推着走、越跑越偏 */
    s_last_ms += LED_HEARTBEAT_MS;

    /* PB2 高电平点亮, 所以直接读输出位取反 */
    GPIO_WriteBit(LED_PORT, LED_PIN,
                  (GPIO_ReadOutputDataBit(LED_PORT, LED_PIN) == Bit_SET) ? Bit_RESET
                                                                        : Bit_SET);
}
