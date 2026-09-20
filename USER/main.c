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
   它是本工程唯一不依赖任何外设的"我活着"指示灯, 排障时先看它。 */
#define LED_PORT        GPIOB
#define LED_PIN         GPIO_Pin_2

static void LED_Conf(void);
static void LED_Blink(uint8_t times);

static void USART_Conf(uint32_t baudrate);
static void Delay_ms(uint32_t ms);
static void USART_SendBytes(const uint8_t *data, uint32_t lenth);      /* 通用: 逐字节发送 */
static void USART_SendStr(const char *s);                              /* 发送常量字符串(长度自动算) */
static void USART_SendHex16(uint16_t v);                               /* 发送 16 位十六进制(排障用) */



int main(void)
{
    /* ---- 第一件事: 让 LED2(PB2) 闪三下 ----
       这一步只碰一个 GPIO, 不依赖串口、不依赖 I2C, 所以它能唯一地回答
       那个最关键的问题 —— "固件到底跑起来没有":
         灯闪三下 -> 固件在跑。串口没输出就纯粹是串口链路的事。
         灯不闪   -> 固件根本没跑, 是烧录/启动的问题, 跟 OLED 无关。 */
    LED_Conf();
    LED_Blink(3U);

    USART_Conf(9600);

    /* 初始化接收环形缓冲 + 使能 USART1 接收非空中断(RXNE):
       来一个字节自动存入 g_uart1_rx(见 System/stm32f4xx_it.c 的 USART1_IRQHandler) */
    RingBuf_Init(&g_uart1_rx, g_uart1_rx_mem, sizeof(g_uart1_rx_mem));
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_EnableIRQ(USART1_IRQn);

    /* ---- 分步报点 ----
       每过一步就往串口打一行, 用来定位到底卡在哪一步。
       一行都没有 -> 固件根本没跑起来, 或者串口本身不通。
       定位完可以删掉, 留着也不占多少 Flash。 */
    USART_SendStr("BOOT 1: main entered\r\n");

    /* ---- 1.44 寸 SPI TFT (ST7735S 128x128) ----
       SCK=PB13  SDA=PB15  CS=PB12  A0=PB10  RESET=PB11, 背光接 3.3V */
    LCD_Init();
    USART_SendStr("BOOT 2: LCD_Init done. SPI2 CR1=");
    USART_SendHex16(LCD_SpiCR1());          /* 正常应是 0x034C */
    USART_SendStr(" SR=");
    USART_SendHex16(LCD_SpiSR());           /* bit8=MODF, 置位说明出过主模式故障 */
    USART_SendStr("\r\n");

    LCD_SelfTest();
    USART_SendStr("BOOT 3: self test drawn\r\n");

    /* ---- 方向试配表 ----
       上下颠倒 -> 换 MADCTL 的 MY 位(bit7); 左右颠倒 -> 换 MX 位(bit6);
       整体偏移或被切边 -> 换 colOff / rowOff。
       每收到一个串口字节就换下一组并重画, 按几下键就能试出这块屏用哪组,
       不用反复改代码 + 烧录。试出来后把那一组填回 LCD.c 的宏即可。 */
    {
        static const uint8_t presets[][3] = {
            /* { MADCTL, colOff, rowOff } —— 按 1.8 寸 128x160 的面板列 */
            { 0xC8U, 0U, 0U },      /* 0: MY=1 MX=1, (0,0) —— 1.8 寸最常见 */
            { 0xC8U, 2U, 1U },      /* 1: MY=1 MX=1, (2,1)(Adafruit green tab) */
            { 0xC8U, 2U, 0U },      /* 2 */
            { 0xC8U, 0U, 1U },      /* 3 */
            { 0x08U, 0U, 0U },      /* 4: MY=0 MX=0 */
            { 0x08U, 2U, 1U },      /* 5 */
            { 0x48U, 0U, 0U },      /* 6: MY=0 MX=1 */
            { 0x88U, 0U, 0U },      /* 7: MY=1 MX=0 */
        };
        uint8_t preset_n = (uint8_t)(sizeof(presets) / sizeof(presets[0]));
        uint8_t preset_i = 0U;
        uint16_t hb = 0U;                            /* 心跳计数 */

        USART_SendStr("Send any key to cycle rotation presets\r\n");

        while (1)
        {
            uint8_t ch;

            if (RingBuf_ReadByte(&g_uart1_rx, &ch))
            {
                uint8_t d;

                USART_SendBytes(&ch, 1U);            /* 回显 */

                /* 换成下一组参数并重画 */
                LCD_SetRotation(presets[preset_i][0], presets[preset_i][1], presets[preset_i][2]);
                LCD_SelfTest();

                USART_SendStr("preset ");
                d = (uint8_t)('0' + preset_i);
                USART_SendBytes(&d, 1U);
                USART_SendStr(": MADCTL=");
                USART_SendHex16(presets[preset_i][0]);
                USART_SendStr(" colOff=");
                USART_SendHex16(presets[preset_i][1]);
                USART_SendStr(" rowOff=");
                USART_SendHex16(presets[preset_i][2]);
                USART_SendStr("\r\n");

                preset_i = (uint8_t)((preset_i + 1U) % preset_n);
            }
            else
            {
                Delay_ms(1U);                        /* 没有数据时休息, 别空转烧 CPU */

                /* 心跳: 约每 500ms 翻转一次 LED2。
                   启动阶段闪完三下之后还在慢慢闪 = 主循环也在正常运转,
                   不只是"启动跑通就卡住了" */
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

/* 发送以 '\0' 结尾的字符串。长度自动数出来, 免得手写常数写错
   (写长了会多发乱码, 写短了会截断)。 */
static void USART_SendStr(const char *s)
{
    uint32_t n = 0U;

    while (s[n] != '\0') { n++; }

    USART_SendBytes((const uint8_t *)s, n);
}

/* 发送 16 位十六进制, 形如 0x034C */
static void USART_SendHex16(uint16_t v)
{
    static const char H[] = "0123456789ABCDEF";
    uint8_t buf[6];

    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = (uint8_t)H[(v >> 12) & 0x0FU];
    buf[3] = (uint8_t)H[(v >> 8)  & 0x0FU];
    buf[4] = (uint8_t)H[(v >> 4)  & 0x0FU];
    buf[5] = (uint8_t)H[v & 0x0FU];

    USART_SendBytes(buf, 6U);
}

/* ======================= LED: 固件心跳 ======================= */
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

/* 闪 times 下。这是"固件到底跑没跑"的唯一可靠证据 ——
   不依赖串口、不依赖 I2C, 只碰一个 GPIO。 */
static void LED_Blink(uint8_t times)
{
    while (times--)
    {
        GPIO_SetBits(LED_PORT, LED_PIN);
        Delay_ms(120U);
        GPIO_ResetBits(LED_PORT, LED_PIN);
        Delay_ms(120U);
    }
}



