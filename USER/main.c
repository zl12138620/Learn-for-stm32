/**
  ******************************************************************************
  * @file    USER/main.c
  * @brief   主程序: USART1 回显(环形缓冲接收) + 旋转编码器计次串口上报
  *           - USART1 @9600, 8N1; 中断接收 -> 环形缓冲 -> 主循环回显
  *           - 编码器 EC11: A相=PB8, B相=PB7; 停止转动约200ms后打印
  *             ENC CW=<正转次数> CCW=<反转次数>
  *           - 上电应先在串口看到开机横幅, 证明串口链路已通
  ******************************************************************************
  */

#include "main.h"
#include "Encoder.h"

/* ---------------- USART1 ---------------- */
#define USART1_IO_CLK   RCC_AHB1Periph_GPIOA
#define USART1_PORT     GPIOA
#define USART1_TX_PIN   GPIO_Pin_9
#define USART1_RX_PIN   GPIO_Pin_10

static void Delay_ms(uint32_t ms);
static void USART1_IO_Conf(void);
static void USART1_Conf(uint32_t baud);
static void Usart_SendString(USART_TypeDef *USARTx, const uint8_t *data, uint32_t dataLen);
static void SendEncoderStatus(void);
static uint16_t U32ToStr(uint32_t v, char *buf);
static uint16_t AppendText(char *dst, const char *src);

/* USART1 接收环形缓冲实例(main.h 有 extern; 中断在 stm32f4xx_it.c 里写入) */
RingBuf_t   g_uart1_rx;
uint8_t     g_uart1_rx_mem[UART1_RX_BUF_SIZE];

/* 编码器已上报标记(每转一格计数变化即上报) */
static uint32_t encLastTotal  = 0U;  /* 上次已上报过的正+反转总次数 */

int main(void)
{
  USART1_IO_Conf();                   /* 串口 IO: PA9=TX, PA10=RX */
  USART1_Conf(9600);                  /* 波特率 9600, 串口助手必须一致 */

  Encoder_Init();                     /* 编码器: PB8=A相, PB7=B相 */

  RingBuf_Init(&g_uart1_rx, g_uart1_rx_mem, sizeof(g_uart1_rx_mem));
  USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);  /* 收到字节触发 RXNE */
  NVIC_EnableIRQ(USART1_IRQn);                    /* 使能 USART1 中断 */

  /* 开机横幅: 串口助手能收到这一行 = 发送链路通 */
  Usart_SendString(USART1, (const uint8_t *)"STM32F407 USART1+Encoder demo\r\n",
                   sizeof("STM32F407 USART1+Encoder demo\r\n") - 1U);

  while (1)
  {
    uint8_t ch;
    uint32_t total;

    if (RingBuf_ReadByte(&g_uart1_rx, &ch))
    {
      Usart_SendString(USART1, &ch, 1U);   /* 回显: 助手发什么收什么 */
    }
    else
    {
      Delay_ms(1U);                        /* 无数据时休息 1ms */
    }

    /* 编码器: 每转一格计数变化就立即上报一次 */
    total = Encoder_GetCW() + Encoder_GetCCW();
    if (total != encLastTotal)
    {
      SendEncoderStatus();                 /* 正转次数：x次，反转次数：x次 */
      encLastTotal = total;
    }
  }
}

/**
  * @brief  轮询 SysTick 实现的毫秒延时(阻塞式, 不依赖 SysTick 中断)
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

static void Delay_us_SysTick(uint32_t us)
{
  SysTick->CTRL = 0;
  SysTick->LOAD = 0x00FFFFFFUL;                 /* 24bit 最大, 自由奔跑 */
  SysTick->VAL  = 0;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

  uint32_t ticks = us * (SystemCoreClock / 1000000U);
  uint32_t start = SysTick->VAL;
  while (((start - SysTick->VAL) & 0x00FFFFFFUL) < ticks) { }
}

/* uint32 -> 十进制字符串(buf 需 >= 11 字节), 返回长度 */
static uint16_t U32ToStr(uint32_t v, char *buf)
{
  char tmp[10];
  uint16_t len = 0U;
  uint16_t i;

  do
  {
    tmp[len++] = (char)('0' + (v % 10U));
    v /= 10U;
  } while (v != 0U);

  for (i = 0U; i < len; i++)
  {
    buf[i] = tmp[len - 1U - i];
  }
  buf[len] = '\0';
  return len;
}

/* 字符串追加到 dst, 返回写入长度 */
static uint16_t AppendText(char *dst, const char *src)
{
  uint16_t n = 0U;

  while (src[n] != '\0')
  {
    dst[n] = src[n];
    n++;
  }
  return n;
}

/* ========================================================================
 * 串口发送一行中文累计值, 例如:  正转次数：5次，反转次数：3次
 * 说明: 下面的 \x 转义是这几个汉字的 GBK(ANSI) 字节编码, 供串口助手
 *       按默认 ANSI/GBK 解码时正常显示; 若你的助手按 UTF-8 解码看到乱码,
 *       在助手里把“接收编码”切到 ANSI/GBK(或把下面换成 UTF-8 字节串)。
 *   "正转次数："  = D5FD D7AA B4CE CAFD A3BA
 *   "次，"       = B4CE 2C(半角逗号)
 *   "反转次数：" = B7B4 D7AA B4CE CAFD A3BA
 *   "次\r\n"     = B4CE 0D 0A
 * ====================================================================== */
static void SendEncoderStatus(void)
{
  char msg[56];
  char s1[11], s2[11];
  uint16_t n = 0U;

  U32ToStr(Encoder_GetCW(),  s1);
  U32ToStr(Encoder_GetCCW(), s2);

  n += AppendText(&msg[n], "\xD5\xFD\xD7\xAA\xB4\xCE\xCA\xFD\xA3\xBA"); /* 正转次数： */
  n += AppendText(&msg[n], s1);
  n += AppendText(&msg[n], "\xB4\xCE,");                                /* 次，        */
  n += AppendText(&msg[n], "\xB7\xB4\xD7\xAA\xB4\xCE\xCA\xFD\xA3\xBA"); /* 反转次数： */
  n += AppendText(&msg[n], s2);
  n += AppendText(&msg[n], "\xB4\xCE\r\n");                             /* 次\r\n      */

  Usart_SendString(USART1, (const uint8_t *)msg, n);
}

/* ========================================================================
 * USART1 IO 配置: PA9=TX(复用推挽), PA10=RX(复用, 上拉)
 * ====================================================================== */
static void USART1_IO_Conf(void)
{
  GPIO_InitTypeDef GPIO_InitStructure;

  RCC_AHB1PeriphClockCmd(USART1_IO_CLK, ENABLE);

  GPIO_PinAFConfig(GPIOA, GPIO_PinSource9,  GPIO_AF_USART1);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

  /* TX: PA9 */
  GPIO_StructInit(&GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin   = USART1_TX_PIN;
  GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
  GPIO_Init(USART1_PORT, &GPIO_InitStructure);

  /* RX: PA10 */
  GPIO_StructInit(&GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin   = USART1_RX_PIN;
  GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
  GPIO_Init(USART1_PORT, &GPIO_InitStructure);
}

/* ========================================================================
 * USART1 配置: 8 数据位, 无校验, 1 停止位, 无流控, 收发都开
 * ====================================================================== */
static void USART1_Conf(uint32_t baud)
{
  USART_InitTypeDef USART_InitStructure;

  RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
  USART_DeInit(USART1);

  USART_StructInit(&USART_InitStructure);
  USART_InitStructure.USART_BaudRate            = baud;
  USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
  USART_InitStructure.USART_StopBits            = USART_StopBits_1;
  USART_InitStructure.USART_Parity              = USART_Parity_No;
  USART_InitStructure.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
  USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  USART_Init(USART1, &USART_InitStructure);
  USART_Cmd(USART1, ENABLE);
}

/* ========================================================================
 * 阻塞发送 dataLen 字节(等待 TXE 再写 DR; 结束后等 TC)
 * ====================================================================== */
static void Usart_SendString(USART_TypeDef *USARTx, const uint8_t *data, uint32_t dataLen)
{
  uint32_t i;

  for (i = 0U; i < dataLen; i++)
  {
    while (USART_GetFlagStatus(USARTx, USART_FLAG_TXE) == RESET)
    {
    }
    USART_SendData(USARTx, data[i]);
  }
  while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET)
  {
  }
}

