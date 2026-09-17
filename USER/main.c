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



#define LED_PORT        GPIOB
#define LED_RCC_CLK     RCC_AHB1Periph_GPIOB
#define LED_PIN         GPIO_Pin_2



static void USART_Conf(uint32_t baudrate);
static void Delay_ms(uint32_t ms);
static void USART_SendBytes(const uint8_t *data, uint32_t lenth);      /* 通用: 逐字节发送 */
static void LED_Conf(void);

static void USART_SendUInt(uint8_t v);        /* 发送 0~255 的十进制(不带前导零) */
static void CMD_Feed(uint8_t ch);             /* 逐字节喂给 AT 命令解析器 */
static void CMD_Execute(const char *line);    /* 执行一整行 AT 命令 */
static void CMD_ReplyAngle(void);             /* 回复 "+ANGLE=<角度>" */
static void CMD_ReplyError(void);             /* 回复 "ERROR" */
static void ENC_ToServo(void);                /* 编码器转动 -> 舵机跟进 */
static void SW_ToServo(void);                 /* 编码器轴按下 -> 舵机回中位 */

/* ============ 编码器 -> 舵机 ============ */
/* 每转一小格(一个定位点)舵机走 5°: 正转加、反转减, 撞到行程端点就停下。 */
#define ENC_STEP_DEG     5

/* SW 按键消抖: 要连续这么多次轮询都读到同一电平才认账。
   主循环空转是微秒级的, 这个数大致对应几毫秒, 足够滤掉按键抖动。 */
#define ENC_SW_DEBOUNCE  8000U

static uint32_t s_enc_last_cw  = 0U;          /* 上次已处理的正转累计值 */
static uint32_t s_enc_last_ccw = 0U;          /* 上次已处理的反转累计值 */
static uint8_t  s_sw_stable    = 1U;          /* SW 消抖后的电平: 1=松开 0=按下 */
static uint16_t s_sw_count     = 0U;          /* SW 电平变化的连续计数 */

/* ============ 舵机 AT 命令解析状态 ============ */
/* 命令必须以 "AT" 开头, 并以 \r 或 \n 结尾。
   用命令前缀而不是裸数字, 是为了避免"我明明只想发一串数据过去, 却意外
   让舵机转起来" —— 裸数字现在没有任何副作用, 一律回 ERROR。
   串口助手请勾选「AT指令自动回车」, 或在发送内容后手动加 \r\n。 */
#define CMD_LINE_MAX  24U                     /* 最长命令 "AT+ANGLE=180" 才 12 字节, 留足余量 */
static char    s_line[CMD_LINE_MAX + 1U];     /* +1 放字符串结束符 */
static uint8_t s_line_len  = 0U;              /* 当前已收字节数 */
static uint8_t s_line_drop = 0U;              /* 本行超长, 整行作废 */






int main(void)
{
    USART_Conf(9600);

    /* 初始化接收环形缓冲 + 使能 USART1 接收非空中断(RXNE):
       来一个字节自动存入 g_uart1_rx(见 System/stm32f4xx_it.c 的 USART1_IRQHandler) */
    RingBuf_Init(&g_uart1_rx, g_uart1_rx_mem, sizeof(g_uart1_rx_mem));
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_EnableIRQ(USART1_IRQn);

    LED_Conf();
    GPIO_WriteBit(LED_PORT, LED_PIN, Bit_SET);   /* PB2 输出高 -> 点亮 LED2(阳极接 PB2, 阴极经 R29 到 GND) */

    Servo_Init();                                /* PA1 + TIM5_CH2, 上电回中位 90° */
    Encoder_Init();                              /* PB6=A相 PB5=B相 PB0=SW + EXTI6 + TIM7 消抖 */

    /* 开机横幅: 串口助手能收到这一行, 就证明"固件确实在跑"且"发送链路通"。
       没有它的话, 串口一片安静时无法区分是固件没跑、还是串口线接错 ——
       本工程 Encoder demo 时代就靠这招排障, 别删。 */
    static const char banner[] =
        "SG90 ready: PA1/TIM5_CH2 50Hz, angle=90\r\n"
        "AT | AT+ANGLE=<0-180> | AT+ANGLE?   (end with CR LF)\r\n"
        "Encoder: turn to nudge +-5deg, press SW to re-center\r\n";
    USART_SendBytes((const uint8_t *)banner, sizeof(banner) - 1U);

    uint8_t rx_echo[UART1_RX_BUF_SIZE];          /* 回显用的中转缓冲 */
    while (1)
    {
        /* ---- 串口: 回显 + AT 命令 ----
           RingBuf_Read 一次取走当前攒下的全部字节, 返回实际取出的个数;
           取不到时返回 0, 这里判一下, 免得空调用 USART_SendBytes 去死等 TC。 */
        uint16_t len = RingBuf_Read(&g_uart1_rx, rx_echo, sizeof(rx_echo));
        if (len > 0U)
        {
            USART_SendBytes(rx_echo, len);       /* 原样回显 */

            for (uint16_t i = 0U; i < len; i++)  /* 同一批字节再喂给命令解析器 */
            {
                CMD_Feed(rx_echo[i]);
            }
        }

        /* ---- 编码器转动 / SW 按键 ----
           都放在主循环里轮询: 计数由 EXTI+TIM7 中断在后台累加, 这里只读结果 */
        ENC_ToServo();
        SW_ToServo();
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

/* 发送 0~255 的十进制, 不补前导零(串口助手上读着清爽)。
   手写而不用 printf/snprintf: 只为打 3 个数字不值得把 stdio 拉进来。 */
static void USART_SendUInt(uint8_t v)
{
    uint8_t buf[3];

    buf[0] = (uint8_t)('0' + (v / 100U));
    buf[1] = (uint8_t)('0' + ((v / 10U) % 10U));
    buf[2] = (uint8_t)('0' + (v % 10U));

    if (v >= 100U)     { USART_SendBytes(buf, 3U); }
    else if (v >= 10U) { USART_SendBytes(&buf[1], 2U); }   /* 跳过百位 */
    else               { USART_SendBytes(&buf[2], 1U); }   /* 只发个位 */
}

/* 大小写不敏感的字符比较(AT 命令惯例, AT 和 at 都认) */
static uint8_t CMD_CharEq(char a, char b)
{
    if ((a >= 'a') && (a <= 'z')) { a = (char)(a - 'a' + 'A'); }
    if ((b >= 'a') && (b <= 'z')) { b = (char)(b - 'a' + 'A'); }
    return (a == b) ? 1U : 0U;
}

/* s 是否以 key 开头(大小写不敏感)。s 提前结束(遇到 \0)也算不匹配 */
static uint8_t CMD_Match(const char *s, const char *key)
{
    while (*key != '\0')
    {
        if (CMD_CharEq(*s, *key) == 0U) { return 0U; }
        s++;
        key++;
    }
    return 1U;
}

static void CMD_ReplyError(void)
{
    USART_SendBytes((const uint8_t *)"ERROR\r\n", 7U);
}

static void CMD_ReplyAngle(void)
{
    USART_SendBytes((const uint8_t *)"+ANGLE=", 7U);
    USART_SendUInt(Servo_GetAngle());
    USART_SendBytes((const uint8_t *)"\r\n", 2U);
}

/* 执行一整行命令(结尾的 \r\n 已被 CMD_Feed 剥掉)。命令集:
 *
 *   AT                   -> OK              握手/连通性测试
 *   AT+ANGLE=<0~180>     -> +ANGLE=<实际值>  设置角度(超范围夹紧)
 *   AT+ANGLE?            -> +ANGLE=<当前值>  回读角度
 *   其它一切              -> ERROR
 *
 * "其它一切回 ERROR" 是有意设计的: 裸数字(比如直接发 90)不会有任何副作用,
 * 这正是改用 AT 前缀的原因 —— 想把 90 当普通数据发出去时不会误转舵机。 */
static void CMD_Execute(const char *line)
{
    const char *p;
    uint16_t    deg;

    if (CMD_Match(line, "AT") == 0U)          /* 不以 AT 开头 */
    {
        CMD_ReplyError();
        return;
    }
    p = line + 2;

    if (*p == '\0')                           /* 光一个 "AT" = 握手 */
    {
        USART_SendBytes((const uint8_t *)"OK\r\n", 4U);
        return;
    }

    if (*p != '+')
    {
        CMD_ReplyError();
        return;
    }
    p++;

    if (CMD_Match(p, "ANGLE") == 0U)
    {
        CMD_ReplyError();
        return;
    }
    p += 5;                                   /* 跳过 "ANGLE" */

    /* ---- AT+ANGLE=<数字> ---- */
    if (*p == '=')
    {
        p++;
        if ((*p < '0') || (*p > '9'))          /* 等号后面不是数字 */
        {
            CMD_ReplyError();
            return;
        }

        deg = 0U;
        while ((*p >= '0') && (*p <= '9'))
        {
            /* 一旦超过 180 就不再累积, 免得两位数乘下去把 uint16 撑溢出 */
            if (deg <= 180U)
            {
                deg = (uint16_t)(deg * 10U + (uint16_t)(*p - '0'));
            }
            p++;
        }

        if (*p != '\0')                        /* 数字后面还跟着别的东西 */
        {
            CMD_ReplyError();
            return;
        }

        if (deg > 180U) { deg = 180U; }        /* 超范围夹紧, 不报错 */
        Servo_SetAngle((uint8_t)deg);
        CMD_ReplyAngle();
        return;
    }

    /* ---- AT+ANGLE? ---- */
    if (*p == '?')
    {
        p++;
        if (*p != '\0')
        {
            CMD_ReplyError();
            return;
        }
        CMD_ReplyAngle();
        return;
    }

    CMD_ReplyError();
}

/* 逐字节喂进 AT 命令解析器: 攒满一整行(遇到 \r 或 \n)才交给 CMD_Execute。
   这样"多发一位/少发一位"只会让命令不合法而回 ERROR, 不会中途转到错误角度。 */
static void CMD_Feed(uint8_t ch)
{
    /* ---- 行结束: 执行本行 ---- */
    if ((ch == '\r') || (ch == '\n'))
    {
        if (s_line_drop != 0U)
        {
            CMD_ReplyError();                  /* 超长: 明确报错, 不静默丢弃 */
        }
        else if (s_line_len > 0U)
        {
            s_line[s_line_len] = '\0';
            CMD_Execute(s_line);
        }
        /* 空行(比如连按两次回车)什么都不回, 免得刷屏 */

        s_line_len  = 0U;
        s_line_drop = 0U;
        return;
    }

    if (s_line_len < CMD_LINE_MAX)
    {
        s_line[s_line_len++] = (char)ch;
    }
    else
    {
        s_line_drop = 1U;                      /* 超长, 本行作废 */
    }
}


/* 读编码器计数变化, 每转一格让舵机走 ENC_STEP_DEG 度。
   用"与上次的差值"而不是"计数是否非零", 这样主循环一轮里连转好几格也不会漏。 */
static void ENC_ToServo(void)
{
    uint32_t cw    = Encoder_GetCW();
    uint32_t ccw   = Encoder_GetCCW();
    int32_t  delta;
    int32_t  angle;

    if ((cw == s_enc_last_cw) && (ccw == s_enc_last_ccw))
    {
        return;                            /* 没转过, 直接返回 */
    }

    /* 正转格数 - 反转格数 = 净转动格数(带符号) */
    delta = (int32_t)(cw - s_enc_last_cw) - (int32_t)(ccw - s_enc_last_ccw);

    s_enc_last_cw  = cw;
    s_enc_last_ccw = ccw;

    angle = (int32_t)Servo_GetAngle() + (delta * ENC_STEP_DEG);

    /* 撞到行程端点就停在端点, 不绕回另一头 */
    if (angle < 0)   { angle = 0; }
    if (angle > 180) { angle = 180; }

    /* 已经在端点还继续往同方向转时角度不变, 那就别刷屏了 */
    if ((uint8_t)angle != Servo_GetAngle())
    {
        Servo_SetAngle((uint8_t)angle);
        CMD_ReplyAngle();                  /* 回 "+ANGLE=<角度>", 便于核验 */
    }
}

/* SW 按键(编码器轴往下按): 按下一次 -> 舵机回中位 90°。
   SW 是上拉输入, 松开为高、按下为低; 连续 ENC_SW_DEBOUNCE 次读到同一
   电平才认账, 这样机械抖动不会一次按下触发好几次。 */
static void SW_ToServo(void)
{
    uint8_t level = (GPIO_ReadInputDataBit(ENC_GPIO_PORT, ENC_SW_PIN) == Bit_RESET) ? 0U : 1U;

    if (level == s_sw_stable)
    {
        s_sw_count = 0U;                   /* 电平稳定, 计数器清零 */
        return;
    }

    s_sw_count++;
    if (s_sw_count < ENC_SW_DEBOUNCE)
    {
        return;                            /* 还没稳够, 继续等 */
    }

    s_sw_count  = 0U;
    s_sw_stable = level;

    if (level == 0U)                       /* 确认按下 */
    {
        Servo_SetAngle(90U);
        CMD_ReplyAngle();
    }
}

static void LED_Conf(void)
{
    RCC_AHB1PeriphClockCmd(LED_RCC_CLK, ENABLE);
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_Pin = LED_PIN;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(LED_PORT,&GPIO_InitStruct);
    
}   




//串口中断任务执行
void USART1_IRQHandler(void)
{
  if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
  {
    uint8_t ch = (uint8_t)USART_ReceiveData(USART1); /* 读 DR 会自动清 RXNE */
    RingBuf_WriteByte(&g_uart1_rx, ch);              /* 缓冲满则自动丢弃该字节 */
  }
}


