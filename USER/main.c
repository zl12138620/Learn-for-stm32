#include "main.h"

/*串口引脚宏定义*/
#define USART1_RCC_CLK  RCC_APB2Periph_USART1
#define USART1_IO_CLK   RCC_AHB1Periph_GPIOA
#define USART1_PORT     GPIOA
#define USART1_TX_PIN   GPIO_Pin_9
#define USART1_RX_PIN    GPIO_Pin_10
 
static void USART_Conf(uint32_t baudrate);
static void Delay_ms(uint32_t ms);
static void USART_TXData(uint16_t *data, uint32_t lenth);



int main(void)
{
    USART_Conf(9600);
    uint16_t data[] = {1,2,3,4,5};
    while (1)
    {
        USART_TXData(data, sizeof(data));
        Delay_ms(1000);/* code */
    }
    
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

static void USART_TXData(uint16_t *data, uint32_t lenth)
{
    while (!USART_GetFlagStatus(USART1, USART_FLAG_TXE) == SET);
    for(uint32_t i=0; i<lenth; i++)
    {
        USART_SendData(USART1, data[i]);
    }
    while (!USART_GetFlagStatus(USART1, USART_FLAG_TC) == SET);
}

static uint16_t USART_RXData(void)
{   
    return USART_ReceiveData(USART1);
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
