/**
  ******************************************************************************
  * @file    SoftWare/src/Usart.c
  * @brief   调试串口 USART1 实现 —— 详见 Usart.h 的说明
  ******************************************************************************
  */

#include "Usart.h"
#include "Ring_buffer.h"

/* ======================= 引脚 ======================= */
#define USART1_RCC_CLK      RCC_APB2Periph_USART1
#define USART1_IO_CLK       RCC_AHB1Periph_GPIOA
#define USART1_PORT         GPIOA
#define USART1_TX_PIN       GPIO_Pin_9
#define USART1_RX_PIN       GPIO_Pin_10

/* ======================= 接收缓冲 ======================= */
/* 生产者 = USART1_IRQHandler(中断上下文), 消费者 = Usart_ReadByte(主循环)。
   两个 static, 外部拿不到 —— 想读就调 Usart_ReadByte(), 这样环形缓冲的
   "读指针只有消费者动、写指针只有生产者动"这条约束不容易被破坏。 */
static RingBuf_t s_rx;
static uint8_t   s_rx_mem[UART1_RX_BUF_SIZE];

/* ======================= 初始化 ======================= */
void Usart_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef        gpio;
    USART_InitTypeDef       uart;

    /* ---- 引脚: PA9=TX / PA10=RX, 复用推挽 ---- */
    RCC_AHB1PeriphClockCmd(USART1_IO_CLK, ENABLE);
    RCC_APB2PeriphClockCmd(USART1_RCC_CLK, ENABLE);

    GPIO_PinAFConfig(USART1_PORT, GPIO_PinSource9,  GPIO_AF_USART1);
    GPIO_PinAFConfig(USART1_PORT, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Pin   = USART1_TX_PIN | USART1_RX_PIN;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(USART1_PORT, &gpio);

    /* ---- 串口本体 ---- */
    USART_StructInit(&uart);
    uart.USART_BaudRate            = baudrate;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    uart.USART_Parity              = USART_Parity_No;
    uart.USART_StopBits            = USART_StopBits_1;
    uart.USART_WordLength          = USART_WordLength_8b;
    USART_Init(USART1, &uart);
    USART_Cmd(USART1, ENABLE);

    /* ---- 接收: 每个字节进环形缓冲, 主循环再取 ---- */
    RingBuf_Init(&s_rx, s_rx_mem, sizeof(s_rx_mem));
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_EnableIRQ(USART1_IRQn);
}

/* ======================= 发送 ======================= */
void Usart_SendBytes(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; i++)
    {
        /* 等 DR 空再写, 否则会覆盖上一字节 */
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { }
        USART_SendData(USART1, data[i]);
    }

    /* 等最后一个字节完全移出移位寄存器。少了这一步, 紧接着复位/掉电
       会把最后一个字节吃掉 —— 排查时看到"少一个字符"会很难想。 */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) { }
}

/* ======================= 接收 ======================= */
uint8_t Usart_ReadByte(uint8_t *ch)
{
    return RingBuf_ReadByte(&s_rx, ch) ? 1U : 0U;
}

/* ========================================================================
 * USART1 中断: 收到一个字节就塞进环形缓冲
 *
 * ⚠ 中断里只做这一件事 —— 打印、解析、状态机全部留给主循环。
 *   在中断里调 Usart_SendBytes() 会阻塞(9600 下一个字节约 1ms),
 *   那段时间其他中断全被拖住, 编码器计数会丢。
 *
 * 这个函数原本在 System/stm32f4xx_it.c 里, 2026-09-22 搬到这儿 ——
 * 中断和它读写的 s_rx 放同一个文件, 免得两处来回跳。
 * ====================================================================== */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        /* 读 DR 会自动清 RXNE 标志 */
        uint8_t ch = (uint8_t)USART_ReceiveData(USART1);

        /* 缓冲满时 RingBuf 自己丢弃 —— 调试串口丢字节无所谓, 不能阻塞 */
        RingBuf_WriteByte(&s_rx, ch);
    }
}
