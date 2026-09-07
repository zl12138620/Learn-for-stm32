#include "main.h"

/* ---------------- 用户按键: WKUP = PA0 ---------------- */
#define KEY_RCC_CLK   RCC_AHB1Periph_GPIOA
#define KEY_PORT      GPIOA
#define KEY_PIN       GPIO_Pin_0

/* ---------------- 用户 LED: BOOT1 = PB2 ---------------- */
#define LED_RCC_CLK   RCC_AHB1Periph_GPIOA
#define LED_PORT      GPIOA
#define LED_PIN       GPIO_Pin_7

/* ---------------- USART1 ---------------- */
#define USART1_CLK      RCC_APB2Periph_USART1
#define USART1_IO_CLK   RCC_AHB1Periph_GPIOA
#define USART1_PORT     GPIOA
#define USART1_RX_PIN   GPIO_Pin_9
#define USART1_TX_PIN   GPIO_Pin_10


static void Delay_ms(uint32_t ms);
static void USER_LED_CONFIG(void);
static uint8_t RB_SelfTest(void);          /* 环形缓冲 API 自检, 返回 0=全部通过 */

/* USART1 接收环形缓冲实例(见 main.h extern; 中断在 stm32f4xx_it.c 里写入) */
RingBuf_t   g_uart1_rx;
uint8_t     g_uart1_rx_mem[UART1_RX_BUF_SIZE];

  
void USART1_IO_Conf(void);//配置串口的IO
void USART1_Conf(uint32_t baud);//配置函数，定义一个形参用于配置波特率
void Usart_SendString(USART_TypeDef* USARTx,const uint8_t *data,uint32_t dataLen);




int main(void)
{

  USART1_IO_Conf();//配置串口的IO
  
  USART1_Conf(9600);//配置波特率: 串口助手必须选择相同的波特率(这里是 9600)
  
  /* ---- 环形缓冲初始化 + 开启 USART1 接收中断 ---- */
  RingBuf_Init(&g_uart1_rx, g_uart1_rx_mem, sizeof(g_uart1_rx_mem));
  USART_ITConfig(USART1, USART_IT_RXNE, ENABLE); /* 每收到 1 字节触发 RXNE */
  NVIC_EnableIRQ(USART1_IRQn);                    /* 使能 USART1 中断通道 */

  /* ---- 测试一: 环形缓冲 API 自检(预期串口打印 RB_SELFTEST: PASS) ---- */
  {
    uint8_t err = RB_SelfTest();

    if (err != 0U)
    {
      uint8_t num[2];

      Usart_SendString(USART1, (const uint8_t *)"RB_SELFTEST: FAIL err=",
                       sizeof("RB_SELFTEST: FAIL err=") - 1U);
      num[0] = (uint8_t)('0' + (err / 10U));   /* 出错步号(<=20)按十进制两位发出 */
      num[1] = (uint8_t)('0' + (err % 10U));
      Usart_SendString(USART1, num, 2U);
      Usart_SendString(USART1, (const uint8_t *)"\r\n", 2U);
    }
    else
    {
      Usart_SendString(USART1, (const uint8_t *)"RB_SELFTEST: PASS\r\n",
                       sizeof("RB_SELFTEST: PASS\r\n") - 1U);
    }
  }

  /* ---- 测试二: 回显。串口助手发什么, 预期原样收到什么 ---- */
  Usart_SendString(USART1, (const uint8_t *)"RB Echo ready, send anything...\r\n",
                   sizeof("RB Echo ready, send anything...\r\n") - 1U);

  while (1)
  {
    uint8_t ch;

    if (RingBuf_ReadByte(&g_uart1_rx, &ch))
    {
      Usart_SendString(USART1, &ch, 1U);   /* 从环形缓冲取出并原样回显 */
    }
    else
    {
      Delay_ms(1U);                        /* 无数据时休息 1ms(降低轮询占用) */
    }
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
  * ========================================================================
  * 环形缓冲功能自检(测试一)
  *   用一个 8 字节小环(容量=7)逐条验证 API, 全部通过返回 0;
  *   任何一步失败返回该步骤号, main 会把它打印成 RB_SELFTEST: FAIL err=xx
  * ========================================================================
  */
static uint8_t RB_SelfTest(void)
{
  static uint8_t mem[8];
  RingBuf_t rb;
  uint8_t   ch;
  uint8_t   i;

  /* ① 初始化 + 空/满初始状态 */
  if (!RingBuf_Init(&rb, mem, sizeof(mem)))
  {
    return 1U;
  }
  if (!RingBuf_IsEmpty(&rb))
  {
    return 2U;
  }
  if (RingBuf_Peek(&rb, &ch))            /* 空缓冲 Peek 应失败 */
  {
    return 3U;
  }
  if (RingBuf_Free(&rb) != (sizeof(mem) - 1U))
  {
    return 4U;
  }

  /* ② 写满 7 字节(A~G), 之后应判满 */
  for (i = 0U; i < 7U; i++)
  {
    if (!RingBuf_WriteByte(&rb, (uint8_t)('A' + i)))
    {
      return 5U;
    }
  }
  if (!RingBuf_IsFull(&rb))
  {
    return 6U;
  }
  if (RingBuf_Used(&rb) != 7U)
  {
    return 7U;
  }

  /* ③ 写满后再写: 必须失败(满则丢弃, 绝不覆盖旧数据) */
  if (RingBuf_WriteByte(&rb, 'X'))
  {
    return 8U;
  }

  /* ④ Peek: 查看队首 A 但不移除 */
  if (!RingBuf_Peek(&rb, &ch) || (ch != 'A'))
  {
    return 9U;
  }
  if (RingBuf_Used(&rb) != 7U)           /* 仍应 7 字节 */
  {
    return 10U;
  }

  /* ⑤ 先读出 A B C */
  for (i = 0U; i < 3U; i++)
  {
    if (!RingBuf_ReadByte(&rb, &ch) || (ch != (uint8_t)('A' + i)))
    {
      return 11U;
    }
  }

  /* ⑥ 再写 a b c: 写指针发生"回绕"(绕回数组开头) */
  for (i = 0U; i < 3U; i++)
  {
    if (!RingBuf_WriteByte(&rb, (uint8_t)('a' + i)))
    {
      return 12U;
    }
  }

  /* ⑦ 连续读出到空, 顺序必须是 D E F G a b c(验证回绕后不乱序) */
  {
    static const uint8_t expect[7] = {'D', 'E', 'F', 'G', 'a', 'b', 'c'};

    for (i = 0U; i < 7U; i++)
    {
      if (!RingBuf_ReadByte(&rb, &ch) || (ch != expect[i]))
      {
        return 13U;
      }
    }
  }
  if (!RingBuf_IsEmpty(&rb))
  {
    return 14U;
  }

  /* ⑧ Discard: 写 3 字节后丢弃前 2, 只剩最后一个 */
  RingBuf_WriteByte(&rb, 0x11);
  RingBuf_WriteByte(&rb, 0x22);
  RingBuf_WriteByte(&rb, 0x33);
  if (RingBuf_Discard(&rb, 2U) != 2U)
  {
    return 15U;
  }
  if (RingBuf_Used(&rb) != 1U)
  {
    return 16U;
  }
  if (!RingBuf_ReadByte(&rb, &ch) || (ch != 0x33))
  {
    return 17U;
  }

  return 0U;                             /* 全部通过 */
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


void USART1_IO_Conf(void)
{
  GPIO_InitTypeDef GPIO_InitStructure;	
  
  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA,ENABLE);	
  
  GPIO_PinAFConfig(GPIOA,GPIO_PinSource9,GPIO_AF_USART1);//IO口用作串口引脚要配置复用模式
  GPIO_PinAFConfig(GPIOA,GPIO_PinSource10,GPIO_AF_USART1);
  
  GPIO_StructInit(&GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin           = GPIO_Pin_9;//TX引脚
  GPIO_InitStructure.GPIO_Mode          = GPIO_Mode_AF;//IO口用作串口引脚要配置复用模式
  GPIO_InitStructure.GPIO_Speed         = GPIO_Speed_100MHz;
  GPIO_InitStructure.GPIO_OType         = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd          = GPIO_PuPd_UP;
  GPIO_Init(GPIOA,&GPIO_InitStructure);
  
  GPIO_StructInit(&GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin           = GPIO_Pin_10;//RX引脚
  GPIO_InitStructure.GPIO_Mode          = GPIO_Mode_AF;
  GPIO_InitStructure.GPIO_Speed         = GPIO_Speed_100MHz;
  GPIO_InitStructure.GPIO_OType         = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd          = GPIO_PuPd_UP;
  GPIO_Init(GPIOA,&GPIO_InitStructure);
}


void USART1_Conf(uint32_t baud)//配置函数，定义一个形参用于配置波特率
{
  USART_InitTypeDef USART_InitStructure;//定义配置串口的结构体变量
  
  
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);//开启串口1的时钟
  
  USART_DeInit(USART1);//大概意思是解除此串口的其他配置
  
  USART_StructInit(&USART_InitStructure);
  USART_InitStructure.USART_BaudRate              = baud;//设置波特率
  USART_InitStructure.USART_WordLength            = USART_WordLength_8b;//字节长度为8bit
  USART_InitStructure.USART_StopBits              = USART_StopBits_1;//1个停止位
  USART_InitStructure.USART_Parity                = USART_Parity_No ;//没有校验位
  USART_InitStructure.USART_Mode                  = USART_Mode_Rx | USART_Mode_Tx;//将串口配置为收发模式
  USART_InitStructure.USART_HardwareFlowControl   = USART_HardwareFlowControl_None; //不提供流控 
  USART_Init(USART1,&USART_InitStructure);//将相关参数初始化给串口1
  USART_Cmd(USART1,ENABLE);//开启串口1
}

void Usart_SendString(USART_TypeDef* USARTx,const uint8_t *data,uint32_t dataLen)
{
  uint32_t i;
  
  for(i = 0;i < dataLen;i ++)
  {
    while (USART_GetFlagStatus(USARTx, USART_FLAG_TXE) == RESET);
    USART_SendData(USARTx,data[i]);//发送数据
  }
  while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET);
}

