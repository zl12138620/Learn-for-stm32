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

  
void USART1_IO_Conf(void);//配置串口的IO
void USART1_Conf(uint32_t baud);//配置函数，定义一个形参用于配置波特率
void Usart_SendString(USART_TypeDef* USARTx,uint8_t *data,uint32_t dataLen);




int main(void)
{

  USART1_IO_Conf();//配置串口的IO
  
  USART1_Conf(9600);//配置波特率: 串口助手必须选择相同的波特率(这里是 9600)
  
  // uint8_t data[] = {1,2,3,4,5};//注意: 要加 [ ] 才是数组; 原写法 data 只是单个 uint8_t(=1),
  //                             //被当指针用后会越界读到地址 0x1~0x5 的随机内容

  while (1)
  {
    Usart_SendString(USART1, "hello", 5);//发送字符串
    Delay_ms(1000);
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

void Usart_SendString(USART_TypeDef* USARTx,uint8_t *data,uint32_t dataLen)
{
  uint32_t i;
  
  for(i = 0;i < dataLen;i ++)
  {
    while (USART_GetFlagStatus(USARTx, USART_FLAG_TXE) == RESET);
    USART_SendData(USARTx,data[i]);//发送数据
  }
  while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET);
}

