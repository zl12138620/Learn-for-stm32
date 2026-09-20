#include "main.h"

/* ============ USART1 接收环形缓冲(生产者: USART1_IRQHandler / 消费者: 主循环) ============ */
/* 定义在 main.c, 对应 main.h 中的 extern 声明; 存储区 64 字节(环形缓冲实际最多缓存 63 字节) */
RingBuf_t g_uart1_rx;
uint8_t   g_uart1_rx_mem[UART1_RX_BUF_SIZE];

/*串口引脚宏定义*/
#define USART1_RCC_CLK  RCC_APB2Periph_USART1
#define USART1_IO_CLK   RCC_AHB1Periph_GPIOA
#define USART1_PORT     GPIOA
#define USART1_TX_PIN   GPIO_Pin_9
#define USART1_RX_PIN    GPIO_Pin_10

/* LED2 挂在 PB2 上: 阳极接 PB2, 阴极经 R29 到 GND —— 高电平点亮。
   主循环里让它慢速闪烁当"心跳": 灯在闪 = 固件活着且主循环在转。
   它不依赖串口、不依赖屏幕, 所以哪天再出问题, 先看它。 */
#define LED_PORT        GPIOB
#define LED_PIN         GPIO_Pin_2

static void LED_Conf(void);

static void USART_Conf(uint32_t baudrate);
static void Delay_ms(uint32_t ms);
static void USART_SendBytes(const uint8_t *data, uint32_t lenth);      /* 通用: 逐字节发送 */

int main(void)
{
    LED_Conf();

    USART_Conf(9600);

    /* 初始化接收环形缓冲 + 使能 USART1 接收非空中断(RXNE):
       来一个字节自动存入 g_uart1_rx(见 System/stm32f4xx_it.c 的 USART1_IRQHandler) */
    RingBuf_Init(&g_uart1_rx, g_uart1_rx_mem, sizeof(g_uart1_rx_mem));
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_EnableIRQ(USART1_IRQn);

    /* ---- 1.8 寸 SPI TFT (ST7735S 128x160) ----
       SCK=PB13  SDA=PB15  CS=PB12  A0=PB10  RESET=PB11, 背光接 3.3V */
    LCD_Init();
    LCD_SelfTest();                 /* 边框 + 三色块 + 三行字: 换屏/换线后一眼验证 */

    uint16_t hb = 0U;               /* 心跳计数 */
    while (1)
    {
        uint8_t ch;

        if (RingBuf_ReadByte(&g_uart1_rx, &ch))
        {
            USART_SendBytes(&ch, 1U);            /* 回显 */
        }
        else
        {
            Delay_ms(1U);                        /* 没有数据时休息, 别空转烧 CPU */

            /* 心跳: 约每 500ms 翻转一次 LED2 */
            if (++hb >= 500U)
            {
                hb = 0U;
                GPIO_WriteBit(LED_PORT, LED_PIN,
                              (GPIO_ReadOutputDataBit(LED_PORT, LED_PIN) == Bit_SET) ? Bit_RESET
                                                                                     : Bit_SET);
            }
        }
    }
}

static void Delay_ms(uint32_t ms)
{
  uint32_t reload = SystemCoreClock / 1000U - 1U;   /* ① 1ms 的装载值 = 167999 */

  while (ms--)                                     /* ② 重复 ms 次 */
  {
    SysTick->LOAD = reload;                        /* ③ 装好 1ms */
    SysTick->VAL  = 0;                             /* ④ 清计数和 COUNTFLAG(关键!) */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk; /* ⑤ 开跑(无中断) */
    while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0)  /* ⑥ 忙等约 1ms */
    {
    }
  }
  SysTick->CTRL = 0;                               /* ⑦ 用完关闭, 归还 SysTick */
}

static void USART_IO_Conf(void)//串口IO初始化
{
    RCC_AHB1PeriphClockCmd(USART1_IO_CLK, ENABLE);
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_Pin = USART1_TX_PIN | USART1_RX_PIN;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(USART1_PORT, &GPIO_InitStruct);
}

static void USART_Conf(uint32_t baudrate)
{
    USART_IO_Conf();
    RCC_APB2PeriphClockCmd(USART1_RCC_CLK, ENABLE);
    GPIO_PinAFConfig(USART1_PORT, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(USART1_PORT, GPIO_PinSource10, GPIO_AF_USART1);

    USART_InitTypeDef USART_InitStruct;
    USART_InitStruct.USART_BaudRate = baudrate;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART1, &USART_InitStruct);
    USART_Cmd(USART1, ENABLE);
}

static void USART_SendBytes(const uint8_t *data, uint32_t lenth)
{
    for(uint32_t i = 0; i < lenth; i++)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);  /* 等 DR 空再写, 避免覆盖上一字节 */
        USART_SendData(USART1, data[i]);
    }
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);       /* 等最后一个字节完全发完 */
}

/* ======================= LED: 心跳指示 ======================= */
static void LED_Conf(void)
{
    GPIO_InitTypeDef g;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    GPIO_StructInit(&g);
    g.GPIO_Pin   = LED_PIN;
    g.GPIO_Mode  = GPIO_Mode_OUT;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    g.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(LED_PORT, &g);

    GPIO_ResetBits(LED_PORT, LED_PIN);      /* 先灭 */
}
