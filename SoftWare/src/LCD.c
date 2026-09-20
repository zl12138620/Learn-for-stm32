/**
  ******************************************************************************
  * @file    SoftWare/src/LCD.c
  * @brief   1.44 寸 SPI TFT 驱动实现 —— ST7735S / 128x128
  *
  *          SPI: 硬件 SPI2, 主机, 8bit, 模式 0(CPOL=0 CPHA=0), MSB first,
  *               波特率 42MHz/4 = 10.5MHz(MISO 不接, 本屏只写不读)
  *
  *          显存写入: ST7735S 是"先设窗口, 再连续灌颜色值"。
  *                    窗口设好后 CS 一直保持低电平, 把 w*h 个 RGB565 一次灌完,
  *                    这样整屏填充只有一次 CS 翻转, 比"一个像素一次事务"快得多。
  ******************************************************************************
  */

#include "LCD.h"
#include "OLED_Font.h"      /* 复用那张 8x16 ASCII 字库 */

/* ======================= 引脚配置 ======================= */
/* 改引脚时下面这些一起改 */
#define LCD_CS_PORT         GPIOB
#define LCD_CS_PIN          GPIO_Pin_12

#define LCD_DC_PORT         GPIOB
#define LCD_DC_PIN          GPIO_Pin_10     /* 模块上标 A0 */

#define LCD_RST_PORT        GPIOB
#define LCD_RST_PIN         GPIO_Pin_11

#define LCD_SPI_PORT        GPIOB
#define LCD_SPI_SCK_PIN     GPIO_Pin_13     /* SPI2_SCK  */
#define LCD_SPI_MOSI_PIN    GPIO_Pin_15     /* SPI2_MOSI */

#define LCD_SPI             SPI2
#define LCD_SPI_CLK         RCC_APB1Periph_SPI2
#define LCD_SPI_AF          GPIO_AF_SPI2

/* APB1 = 42MHz。SPI2 最高只能到 APB1/2 = 21MHz, 所以 2 分频就是上限了。
   整屏 128x160x2 = 40960 字节, 21MHz 下约 15.6ms。
   ⚠ 杜邦线较长/接触不良时 21MHz 可能花屏, 那就退回 SPI_BaudRatePrescaler_4
   (10.5MHz, 约 31ms)。再想快只能换到 APB2 上的 SPI1(可到 42MHz),
   但 SPI1 的 PA5/PA7 和板载 SPI FLASH 共用, 要挪线。 */
#define LCD_SPI_PRESCALER   SPI_BaudRatePrescaler_2

/* ======================= 显存偏移 ======================= */
/* ⚠ 这两个值是"可见区的左上角在 ST7735S 显存里对应的列/行"。
   1.44 寸 128x128 面板的可见区通常不从 (0,0) 开始, 所以要补一个偏移。
   判断方法: 跑 LCD_SelfTest(), 边框的右边/下边被切掉 -> 偏移偏小;
             左边/上边出现花边或错位      -> 偏移偏大。
   常见取值: (2,1) 或 (0,0)。 */
/* 1.8 寸 128x160 常见用 (0,0); 若画面整体偏移/被切边就调这里,
   或者用 LCD_SetRotation() 在运行时一组组试。 */
#define LCD_COL_OFFSET      0U
#define LCD_ROW_OFFSET      0U

/* 运行时可变(见 LCD_SetRotation): 上电先取上面的默认值,
   之后 main.c 可以用串口按键一组一组试着换, 不用反复烧录 */
static uint8_t s_col_off = LCD_COL_OFFSET;
static uint8_t s_row_off = LCD_ROW_OFFSET;

/* ======================= 引脚操作 ======================= */
#define LCD_CS_LOW()        GPIO_ResetBits(LCD_CS_PORT, LCD_CS_PIN)

/* 切换 A0(命令/数据)。
   ⚠ 每次切换前必须先等上一字节真正移完:
   SPI_WriteByte 只等到 TXE(发送缓冲空), 而那一刻最后一个字节其实还在移位
   寄存器里往外走。这时候改 A0, 那个字节会被 ST7735S 按新含义解释 ——
   命令整体错位, 表现就是花屏、只显示一部分、或者什么也看不到。
   所以下面两个宏里都夹了一个 SPI_WaitDone()。(函数定义在本文件后面,
   宏在使用处展开, 所以顺序没问题。) */
#define LCD_DC_CMD()        do { SPI_WaitDone(); GPIO_ResetBits(LCD_DC_PORT, LCD_DC_PIN); } while (0)  /* A0=0 命令 */
#define LCD_DC_DATA()       do { SPI_WaitDone(); GPIO_SetBits(LCD_DC_PORT,   LCD_DC_PIN); } while (0)  /* A0=1 数据 */

/* ======================= 毫秒延时 ======================= */
static void LCD_Delay_ms(uint32_t ms)
{
	volatile uint32_t i;

	while (ms--)
	{
		i = 40000;              /* 约 1ms @168MHz; volatile 防止被 -O2 优化掉 */
		while (i--) { }
	}
}

/* ======================= SPI 底层 ======================= */
static void SPI_WriteByte(uint8_t d)
{
	while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_TXE) == RESET) { }
	SPI_I2S_SendData(LCD_SPI, d);
}

/* 等最后一位真正移出去。改 CS/DC 之前必须等, 否则最后几个字节会被截断 */
static void SPI_WaitDone(void)
{
	while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_BSY) == SET) { }
}

/* 结束一次 SPI 事务: 等发完再抬 CS */
static void LCD_EndTransfer(void)
{
	SPI_WaitDone();
	GPIO_SetBits(LCD_CS_PORT, LCD_CS_PIN);
}

/* ======================= 数据宽度切换 ======================= */
/* 命令走 8 位, 像素走 16 位 —— 16 位模式下一次 DMA 传输刚好一个像素。
   ⚠ 参考手册明确要求 DFF 位只能在 SPE=0 时改, 所以下面两个函数必须在
   CS 已经抬高的间隙调用。千万不要在灌像素的中途调, 否则会切坏这一屏。 */
static void LCD_DataSet8(void)
{
	SPI_Cmd(LCD_SPI, DISABLE);
	SPI_DataSizeConfig(LCD_SPI, SPI_DataSize_8b);
	SPI_Cmd(LCD_SPI, ENABLE);
}

static void LCD_DataSet16(void)
{
	SPI_Cmd(LCD_SPI, DISABLE);
	SPI_DataSizeConfig(LCD_SPI, SPI_DataSize_16b);
	SPI_Cmd(LCD_SPI, ENABLE);
}

/* 16 位模式下发一个像素(CPU 方式, 画字用) */
static void SPI_WriteHalf(uint16_t d)
{
	while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_TXE) == RESET) { }
	SPI_I2S_SendData(LCD_SPI, d);
}

/* ======================= DMA ======================= */
/* SPI2_TX 的 DMA 请求在芯片内部固定接在 DMA1 Stream4 Channel0
   (见参考手册的 DMA 请求映射表), 不能自己挑。 */
#define LCD_DMA             DMA1
#define LCD_DMA_STREAM      DMA1_Stream4
#define LCD_DMA_CHANNEL     DMA_Channel_0
#define LCD_DMA_TC_FLAG     DMA_FLAG_TCIF4

/* DMA 的源地址。配合下面的 DMA_MemoryInc_Disable: 整个传输过程中它一直
   读这同一个地址, 于是"一个颜色值重复 N 次"就成了一整片纯色填充 ——
   不需要为整屏准备 40KB 缓冲区。 */
static uint16_t s_dma_color = 0U;

static void LCD_DmaInit(void)
{
	DMA_InitTypeDef dma;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
	DMA_DeInit(LCD_DMA_STREAM);

	dma.DMA_Channel            = LCD_DMA_CHANNEL;
	dma.DMA_PeripheralBaseAddr = (uint32_t)&(LCD_SPI->DR);
	dma.DMA_Memory0BaseAddr    = (uint32_t)&s_dma_color;
	dma.DMA_DIR                = DMA_DIR_MemoryToPeripheral;
	dma.DMA_BufferSize         = 1U;
	dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
	dma.DMA_MemoryInc          = DMA_MemoryInc_Disable;   /* ← 关键: 一直读同一地址 */
	dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
	dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;
	dma.DMA_Mode               = DMA_Mode_Normal;
	dma.DMA_Priority           = DMA_Priority_High;
	dma.DMA_FIFOMode           = DMA_FIFOMode_Disable;
	dma.DMA_FIFOThreshold      = DMA_FIFOThreshold_HalfFull;
	dma.DMA_MemoryBurst        = DMA_MemoryBurst_Single;
	dma.DMA_PeripheralBurst    = DMA_PeripheralBurst_Single;
	DMA_Init(LCD_DMA_STREAM, &dma);
}

/* 用 DMA 把 s_dma_color 连发 pixels 次。返回时数据已全部交给 SPI。
   一次最多 65535 个像素(DMA 计数器是 16 位), 整屏 20480 个够用。 */
static void LCD_DmaSend(uint32_t pixels)
{
	DMA_Cmd(LCD_DMA_STREAM, DISABLE);
	while (DMA_GetCmdStatus(LCD_DMA_STREAM) != DISABLE) { }

	/* ⚠ 这个版本的库, DMA_ClearFlag / DMA_GetFlagStatus 的第一个参数是
	   "流"(DMA_Stream_TypeDef*), 不是"DMA 控制器"。传成 DMA1 编译能过
	   但类型不匹配, 标志位读写就是错的。 */
	DMA_ClearFlag(LCD_DMA_STREAM, LCD_DMA_TC_FLAG);
	DMA_SetCurrDataCounter(LCD_DMA_STREAM, (uint16_t)pixels);
	DMA_Cmd(LCD_DMA_STREAM, ENABLE);

	while (DMA_GetFlagStatus(LCD_DMA_STREAM, LCD_DMA_TC_FLAG) == RESET) { }
	DMA_ClearFlag(LCD_DMA_STREAM, LCD_DMA_TC_FLAG);
}

/* ======================= 命令 / 数据 ======================= */
/* 一条命令 + n 个参数, 整个事务里 CS 保持低 */
static void LCD_CmdN(uint8_t cmd, const uint8_t *params, uint8_t n)
{
	uint8_t i;

	LCD_CS_LOW();
	LCD_DC_CMD();
	SPI_WriteByte(cmd);

	if (n > 0U)
	{
		LCD_DC_DATA();
		for (i = 0U; i < n; i++)
		{
			SPI_WriteByte(params[i]);
		}
	}
	LCD_EndTransfer();
}

/* 无参命令 */
static void LCD_Cmd(uint8_t cmd)
{
	LCD_CmdN(cmd, 0, 0U);
}

/* 单参命令 */
static void LCD_Cmd1(uint8_t cmd, uint8_t p1)
{
	LCD_CmdN(cmd, &p1, 1U);
}

/* ========================================================================
 * 开始一次"写像素":
 *   1) 8 位模式下把窗口设好、发 0x2C
 *   2) 抬 CS, 切到 16 位(手册要求 DFF 只能在 SPE=0 时改, 所以必须在
 *      CS 抬高的间隙做 —— 见 LCD_DataSet16 的说明)
 *   3) 重新拉低 CS、A0 置"数据", 交给调用方灌像素
 * 写完调 LCD_PixelWriteEnd()。
 *
 * 中间抬一次 CS 是安全的: ST7735S 收到 0x2C 后会一直等着收像素,
 * 抬 CS 不会让它丢掉这个状态。
 * ====================================================================== */
static void LCD_PixelWriteBegin(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint16_t x0 = (uint16_t)(x + s_col_off);
	uint16_t x1 = (uint16_t)(x + w - 1U + s_col_off);
	uint16_t y0 = (uint16_t)(y + s_row_off);
	uint16_t y1 = (uint16_t)(y + h - 1U + s_row_off);

	/* ---- 8 位: 设窗口 ---- */
	LCD_CS_LOW();

	LCD_DC_CMD();  SPI_WriteByte(0x2A);
	LCD_DC_DATA(); SPI_WriteByte((uint8_t)(x0 >> 8)); SPI_WriteByte((uint8_t)x0);
	               SPI_WriteByte((uint8_t)(x1 >> 8)); SPI_WriteByte((uint8_t)x1);

	LCD_DC_CMD();  SPI_WriteByte(0x2B);
	LCD_DC_DATA(); SPI_WriteByte((uint8_t)(y0 >> 8)); SPI_WriteByte((uint8_t)y0);
	               SPI_WriteByte((uint8_t)(y1 >> 8)); SPI_WriteByte((uint8_t)y1);

	/* 开始写显存。0x2C 是"命令", 后面灌的全是"数据", 所以发完必须把 A0
	   切回 1 —— 漏了这一步, 几万个像素值会被当成命令, 屏幕就是一片花。 */
	LCD_DC_CMD();  SPI_WriteByte(0x2C);
	LCD_EndTransfer();                  /* 抬 CS */

	/* ---- 此时 CS 是高电平, 可以安全地切数据宽度 ---- */
	LCD_DataSet16();

	/* ---- 重新拉低 CS, A0 置"数据", 交给调用方灌像素 ---- */
	LCD_CS_LOW();
	LCD_DC_DATA();
}

/* 结束一次"写像素": 等数据发完 -> 抬 CS -> 切回 8 位给下一条命令用 */
static void LCD_PixelWriteEnd(void)
{
	SPI_WaitDone();
	LCD_EndTransfer();                  /* 抬 CS */
	LCD_DataSet8();                     /* 切回 8 位 */
}

/* ======================= 初始化 ======================= */
void LCD_Init(void)
{
	GPIO_InitTypeDef gpio;
	SPI_InitTypeDef  spi;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
	RCC_APB1PeriphClockCmd(LCD_SPI_CLK, ENABLE);

	/* ---- 控制脚: CS / A0 / RESET 都是普通推挽输出 ---- */
	GPIO_StructInit(&gpio);
	gpio.GPIO_Mode  = GPIO_Mode_OUT;
	gpio.GPIO_OType = GPIO_OType_PP;
	gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	gpio.GPIO_Pin   = LCD_CS_PIN | LCD_DC_PIN | LCD_RST_PIN;
	GPIO_Init(GPIOB, &gpio);

	GPIO_SetBits(LCD_CS_PORT, LCD_CS_PIN);      /* CS 先抬起来 */

	/* ---- SCK / MOSI 复用成 SPI2 ---- */
	GPIO_PinAFConfig(LCD_SPI_PORT, GPIO_PinSource13, LCD_SPI_AF);
	GPIO_PinAFConfig(LCD_SPI_PORT, GPIO_PinSource15, LCD_SPI_AF);

	gpio.GPIO_Mode  = GPIO_Mode_AF;
	gpio.GPIO_OType = GPIO_OType_PP;
	gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	gpio.GPIO_Pin   = LCD_SPI_SCK_PIN | LCD_SPI_MOSI_PIN;
	GPIO_Init(LCD_SPI_PORT, &gpio);

	/* ---- SPI2: 主机, 8bit, 模式0, 软件 NSS ---- */
	SPI_I2S_DeInit(LCD_SPI);
	spi.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;  /* MISO 不接, 不用管 */
	spi.SPI_Mode              = SPI_Mode_Master;
	spi.SPI_DataSize          = SPI_DataSize_8b;
	spi.SPI_CPOL              = SPI_CPOL_Low;
	spi.SPI_CPHA              = SPI_CPHA_1Edge;
	spi.SPI_NSS               = SPI_NSS_Soft;
	spi.SPI_BaudRatePrescaler = LCD_SPI_PRESCALER;
	spi.SPI_FirstBit          = SPI_FirstBit_MSB;
	spi.SPI_CRCPolynomial     = 7U;
	SPI_Init(LCD_SPI, &spi);

	/* ⚠ 必须补上 SSI, 否则 SPI 一个字节都发不出去!
	   StdPeriph 的 SPI_NSS_Soft 只置了 SSM 位(bit9), 没置 SSI 位(bit8)。
	   SSM=1 时内部 NSS 由 SSI 决定, SSI=0 就等于"片选被拉低"; 而主机模式下
	   NSS 被拉低会触发主模式故障(MODF), 硬件自动清掉 MSTR 把 SPI 变成从机,
	   SCK 从此不再产生任何脉冲。
	   表现极具迷惑性: 代码全在跑、串口正常、背光也亮, 但屏幕什么都不显示
	   (TFT 没被初始化, 背光透出来就是全白)。
	   置上 SSI = "软件片选, 内部认为已选中自己"。 */
	LCD_SPI->CR1 |= SPI_CR1_SSI;

	SPI_Cmd(LCD_SPI, ENABLE);

	/* ---- DMA: 整片纯色填充交给它, CPU 不用一个字节一个字节地喂 ---- */
	LCD_DmaInit();

	/* ⚠ 光配好 DMA 还不够, 必须同时在 SPI 侧打开 TX 的 DMA 请求(CR2 的
	   TXDMAEN 位)。少了这一行, SPI 永远不会向 DMA 发请求 -> DMA 一次都
	   搬不动 -> LCD_DmaSend 里等 TC 标志的那个 while 死等, 现象是固件
	   整个卡死在 LCD_SelfTest 里, LED 心跳停、串口失联。 */
	SPI_I2S_DMACmd(LCD_SPI, SPI_I2S_DMAReq_Tx, ENABLE);

	/* ---- 硬件复位: 手册要求 RESET 低电平至少保持 10us ---- */
	GPIO_ResetBits(LCD_RST_PORT, LCD_RST_PIN);
	LCD_Delay_ms(20U);
	GPIO_SetBits(LCD_RST_PORT, LCD_RST_PIN);
	LCD_Delay_ms(120U);

	/* ================= ST7735S 初始化序列 ================= */
	LCD_Cmd(0x01);                              /* 软件复位 */
	LCD_Delay_ms(150U);

	LCD_Cmd(0x11);                              /* 退出睡眠 */
	LCD_Delay_ms(150U);

	/* 多路复用率(驱动多少行): 160 行 -> 160-1 = 0x9F。
	   1.44 寸 128 行的屏这里是 0x7F。设错了会有一截屏不显示。 */
	LCD_Cmd1(0xA8, 0x9F);

	/* ---- 帧率控制 ---- */
	{ static const uint8_t p[] = { 0x01, 0x2C, 0x2D };              LCD_CmdN(0xB1, p, 3U); }
	{ static const uint8_t p[] = { 0x01, 0x2C, 0x2D };              LCD_CmdN(0xB2, p, 3U); }
	{ static const uint8_t p[] = { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D }; LCD_CmdN(0xB3, p, 6U); }
	{ static const uint8_t p[] = { 0x07 };                          LCD_CmdN(0xB4, p, 1U); }

	/* ---- 电源 ---- */
	{ static const uint8_t p[] = { 0xA2, 0x02, 0x84 };              LCD_CmdN(0xC0, p, 3U); }
	{ static const uint8_t p[] = { 0xC5 };                          LCD_CmdN(0xC1, p, 1U); }
	{ static const uint8_t p[] = { 0x0A, 0x00 };                    LCD_CmdN(0xC2, p, 2U); }
	{ static const uint8_t p[] = { 0x8A, 0x2A };                    LCD_CmdN(0xC3, p, 2U); }
	{ static const uint8_t p[] = { 0x8A, 0xEE };                    LCD_CmdN(0xC4, p, 2U); }
	{ static const uint8_t p[] = { 0x0E };                          LCD_CmdN(0xC5, p, 1U); }

	LCD_Cmd(0x20);                              /* 关闭反显(InverOff) */

	/* 扫描方向 + 颜色顺序(MADCTL):
	     bit7 MY  行方向: 0=正常 1=上下翻转
	     bit6 MX  列方向: 0=正常 1=左右镜像
	     bit3 RGB 颜色顺序: 0=RGB 1=BGR
	   ⚠ 这块 1.8 寸屏实测要用 0xC0(MY=1 MX=1 RGB=0)。

	   ⚠⚠ 排查方向时最容易搞混的一点: **色块全是矩形, 镜像只改变它的位置、
	   不改变它的样子**。所以"左到右是蓝绿红"这句话, 两种完全不同的原因
	   都会产生:
	     位置镜像(MX)   -> 蓝绿红, 而且**文字也是反的**
	     红蓝互换(RGB)  -> 蓝绿红, 但**文字是正常的**
	   **唯一能分辨的是文字**:
	     文字镜像  -> 改 MX(bit6)
	     文字正常  -> 改 RGB(bit3)
	   只看色块顺序就下手, 会像我一样改错位、来回绕好几轮。 */
	LCD_Cmd1(0x36, 0xC0);                       /* MY=1 MX=1 RGB=0 */
	LCD_Cmd1(0x3A, 0x05);                       /* 像素格式: 16bit RGB565 */

	/* ---- 伽马校正 ---- */
	{ static const uint8_t p[] = { 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
	                               0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 };
	  LCD_CmdN(0xE0, p, 16U); }
	{ static const uint8_t p[] = { 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
	                               0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 };
	  LCD_CmdN(0xE1, p, 16U); }

	LCD_Cmd(0x13);                              /* 正常显示模式 */
	LCD_Cmd(0x29);                              /* 开显示 */
	LCD_Delay_ms(50U);
}

/* 运行时改扫描方向和显存偏移(排障用)。
   madctl: MADCTL(0x36) 的值; colOff/rowOff: 显存列/行偏移。
   改完重发 0x36, 之后再画的图就按新参数走。 */
void LCD_SetRotation(uint8_t madctl, uint8_t colOff, uint8_t rowOff)
{
	s_col_off = colOff;
	s_row_off = rowOff;
	LCD_Cmd1(0x36, madctl);
}

/* ======================= 绘图 ======================= */
void LCD_Fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	uint32_t n;

	if ((x >= LCD_W) || (y >= LCD_H) || (w == 0U) || (h == 0U)) { return; }
	if ((uint32_t)x + w > LCD_W) { w = (uint16_t)(LCD_W - x); }
	if ((uint32_t)y + h > LCD_H) { h = (uint16_t)(LCD_H - y); }

	n = (uint32_t)w * (uint32_t)h;

	LCD_PixelWriteBegin(x, y, w, h);

	/* 整片纯色交给 DMA: 同一个颜色值重复 n 次。配合 DMA_MemoryInc_Disable,
	   CPU 在整个传输期间不用管一个字节 —— 现在这里为了流程清楚还是等它
	   传完, 想并行就把 LCD_DmaSend 里的等待挪走、用 DMA 传输完成中断收尾。 */
	s_dma_color = color;
	LCD_DmaSend(n);

	LCD_PixelWriteEnd();
}

void LCD_Clear(uint16_t color)
{
	LCD_Fill(0U, 0U, LCD_W, LCD_H, color);
}

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
	LCD_Fill(x, y, 1U, 1U, color);
}

void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  uint16_t color, uint8_t filled)
{
	if (filled != 0U)
	{
		LCD_Fill(x, y, w, h, color);
		return;
	}

	LCD_Fill(x, y, w, 1U, color);                       /* 上 */
	LCD_Fill(x, (uint16_t)(y + h - 1U), w, 1U, color);  /* 下 */
	LCD_Fill(x, y, 1U, h, color);                       /* 左 */
	LCD_Fill((uint16_t)(x + w - 1U), y, 1U, h, color);  /* 右 */
}

/* ======================= 文字 ======================= */
/* 字库是 8x16: 每个字符 16 字节, 前 8 字节上半部分, 后 8 字节下半部分 */
void LCD_ShowChar(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg)
{
	uint8_t col, row;
	uint8_t idx;

	if ((ch < ' ') || (ch > '~')) { ch = '?'; }     /* 字库只覆盖可见 ASCII */
	idx = (uint8_t)(ch - ' ');

	if (((uint32_t)x + 8U > LCD_W) || ((uint32_t)y + 16U > LCD_H)) { return; }

	LCD_PixelWriteBegin(x, y, 8U, 16U);

	/* ⚠ 这张字库的两个坑, 都踩过:
	   1) 它是**按列**存的, 不是按行 —— 一个字节 = 一列的 8 个像素。当成
	      "一个字节 = 一行 8 个横向像素"去画, 每个字会被转置成乱码。
	   2) 字节里** bit0 在最上面**, bit7 在最下面(SSD1306 的页结构就是这样,
	      江科大那套 OLED 驱动一次写一个字节正好对应一整列, 所以字库按它存)。
	      把 bit7 当第一行, 每个字会上下颠倒 —— 而且 g/p/y 这些带下伸部的
	      字母和下划线会被翻到上面去, 看着像"显示不全"。
	   验证方法: 把 '_' 画出来, 应该在**最后一行**而不是中间。
	   结构: [0..7]  上半部分, 一个字节一列
	         [8..15] 下半部分, 同样一个字节一列 */
	for (row = 0U; row < 16U; row++)
	{
		for (col = 0U; col < 8U; col++)
		{
			uint8_t  b = OLED_F8x16[idx][(row < 8U) ? col : (uint8_t)(col + 8U)];
			uint16_t c = ((b & (uint8_t)(0x01U << (row & 7U))) != 0U) ? fg : bg;
			SPI_WriteHalf(c);           /* 16 位模式下一次写一个像素 */
		}
	}

	LCD_PixelWriteEnd();
}

void LCD_ShowString(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
	while (*s != '\0')
	{
		LCD_ShowChar(x, y, *s, fg, bg);
		x = (uint16_t)(x + 8U);
		if ((uint32_t)x + 8U > LCD_W) { break; }    /* 到右边就停, 不绕行 */
		s++;
	}
}

void LCD_ShowInt(uint16_t x, uint16_t y, int32_t v, uint16_t fg, uint16_t bg)
{
	char     buf[12];
	uint8_t  n = 0U;
	uint32_t u;
	uint8_t  neg = 0U;

	if (v < 0) { neg = 1U; u = (uint32_t)(-(int64_t)v); }
	else       { u = (uint32_t)v; }

	do {
		buf[n++] = (char)('0' + (u % 10U));
		u /= 10U;
	} while ((u != 0U) && (n < 11U));

	if (neg != 0U) { buf[n++] = '-'; }

	/* buf 里是倒着的, 反过来画 */
	while (n > 0U)
	{
		LCD_ShowChar(x, y, buf[--n], fg, bg);
		x = (uint16_t)(x + 8U);
	}
}

/* ======================= 自检 ======================= */
/* 画一圈边框 + 三色块 + 一行字。
   边框是判断"显存偏移对不对"的关键:
     右边/下边的框线看不见 -> LCD_COL_OFFSET / LCD_ROW_OFFSET 偏小, 调大
     左上角出现别的残影     -> 偏移偏大, 调小 */
void LCD_SelfTest(void)
{
	LCD_Clear(LCD_BLACK);

	LCD_DrawRect(0U, 0U, LCD_W, LCD_H, LCD_WHITE, 0U);      /* 整屏白边框 */

	/* 方向指示块: 左上角黄、右下角青。
	   矩形本身没有方向, 但**位置**有 —— 黄块跑到右上角 = 左右镜像,
	   跑到左下角 = 上下颠倒。别小看这两块, 中间那三个色块是分不出
	   "位置镜像"和"红蓝互换"的(两种原因产生的色块顺序一模一样)。 */
	LCD_DrawRect(4U, 4U, 14U, 14U, LCD_YELLOW, 1U);
	LCD_DrawRect(LCD_W - 18U, LCD_H - 18U, 14U, 14U, LCD_CYAN, 1U);

	/* 三个色块: 红绿蓝。注意配合上面两块一起看 */
	LCD_DrawRect(6U,  30U, 36U, 36U, LCD_RED,   1U);
	LCD_DrawRect(46U, 30U, 36U, 36U, LCD_GREEN, 1U);
	LCD_DrawRect(86U, 30U, 36U, 36U, LCD_BLUE,  1U);

	LCD_ShowString(8U, 100U, "ST7735S", LCD_WHITE, LCD_BLACK);
	LCD_ShowString(8U, 120U, "128x160", LCD_YELLOW, LCD_BLACK);
	LCD_ShowString(8U, 140U, "SPI2 PB13/15", LCD_CYAN, LCD_BLACK);
}
