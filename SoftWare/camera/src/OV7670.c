/**
  ******************************************************************************
  * @file    SoftWare/src/OV7670.c
  * @brief   OV7670(带 AL422B FIFO)驱动实现 —— 详见 OV7670.h 的接线的说明
  *
  *          三条来自数据手册的关键结论(踩之前先看):
  *
  *          1) AL422B 手册: "Read data output at the rising edge of the RCK cycle
  *             ... the internal read address is incremented automatically"
  *             -> **数据在 RCK 上升沿输出, 同时读指针自增**。所以每读一个字节
  *                就发一个上升沿。
  *
  *          2) AL422B 手册: "/RRST ... initializes the read address to 0, and is
  *             fetched at the rising edge of the RCK input cycle"
  *             -> **RRST 必须在 RCK 的上升沿被采样才生效**! 只把 RRST 拉低再
  *                拉高是没用的, 必须伴着一次时钟。写指针 WRST 同理。
  *                这是个很隐蔽的坑, 忘了它读出来的图像会从 FIFO 里的随机位置开始。
  *
  *          3) 读指针"regardless of OE"都在走 —— 所以 /OE 只管数据线是否输出,
  *             不控制指针。每帧读之前必须复位读指针。
  ******************************************************************************
  */

#include "OV7670.h"
#include "Tick.h"           /* 抓帧超时要用毫秒时基 */

/* ======================= 引脚 ======================= */
/* SIO_C / SIO_D: SCCB 控制口(I2C 的简化版) */
#define SCCB_SCL_PORT       GPIOE
#define SCCB_SCL_PIN        GPIO_Pin_15
#define SCCB_SDA_PORT       GPIOB
#define SCCB_SDA_PIN        GPIO_Pin_0

/* D0~D7 全挂在 PE 低 8 位 —— 一次 GPIOE->IDR 读回一整个字节 */
#define CAM_DATA_PORT       GPIOE
#define CAM_DATA_MASK       0x00FFU

/* FIFO 控制(AL422B 的控制脚全部低有效) */
#define FIFO_RCK_PORT       GPIOE
#define FIFO_RCK_PIN        GPIO_Pin_8
#define FIFO_OE_PORT        GPIOE
#define FIFO_OE_PIN         GPIO_Pin_9
#define FIFO_RRST_PORT      GPIOE
#define FIFO_RRST_PIN       GPIO_Pin_10
#define FIFO_WRST_PORT      GPIOE
#define FIFO_WRST_PIN       GPIO_Pin_11
#define FIFO_WR_PORT        GPIOE
#define FIFO_WR_PIN         GPIO_Pin_12

/* 摄像头控制 */
#define CAM_RST_PORT        GPIOE
#define CAM_RST_PIN         GPIO_Pin_13
#define CAM_PWDN_PORT       GPIOE
#define CAM_PWDN_PIN        GPIO_Pin_14

/* 帧同步(输入) */
#define CAM_VSYNC_PORT      GPIOB
#define CAM_VSYNC_PIN       GPIO_Pin_1

/* ======================= 引脚操作 ======================= */
#define SET_H(port, pin)    GPIO_SetBits(port, pin)
#define SET_L(port, pin)    GPIO_ResetBits(port, pin)

#define FIFO_RCK_H()        SET_H(FIFO_RCK_PORT,  FIFO_RCK_PIN)
#define FIFO_RCK_L()        SET_L(FIFO_RCK_PORT,  FIFO_RCK_PIN)
#define FIFO_OE_H()         SET_H(FIFO_OE_PORT,   FIFO_OE_PIN)     /* 高 = 不输出 */
#define FIFO_OE_L()         SET_L(FIFO_OE_PORT,   FIFO_OE_PIN)     /* 低 = 输出使能 */
#define FIFO_WR_H()         SET_H(FIFO_WR_PORT,   FIFO_WR_PIN)     /* 高 = 允许捕获 */
#define FIFO_WR_L()         SET_L(FIFO_WR_PORT,   FIFO_WR_PIN)
#define FIFO_WRST_H()       SET_H(FIFO_WRST_PORT, FIFO_WRST_PIN)
#define FIFO_WRST_L()       SET_L(FIFO_WRST_PORT, FIFO_WRST_PIN)
#define FIFO_RRST_H()       SET_H(FIFO_RRST_PORT, FIFO_RRST_PIN)
#define FIFO_RRST_L()       SET_L(FIFO_RRST_PORT, FIFO_RRST_PIN)

#define SCCB_SCL_H()        SET_H(SCCB_SCL_PORT, SCCB_SCL_PIN)
#define SCCB_SCL_L()        SET_L(SCCB_SCL_PORT, SCCB_SCL_PIN)
#define SCCB_SDA_H()        SET_H(SCCB_SDA_PORT, SCCB_SDA_PIN)
#define SCCB_SDA_L()        SET_L(SCCB_SDA_PORT, SCCB_SDA_PIN)
#define SCCB_SDA_READ()     GPIO_ReadInputDataBit(SCCB_SDA_PORT, SCCB_SDA_PIN)

/* ======================= 延时 ======================= */
static void CAM_Delay_us(uint32_t us)          /* 约 0.5us 一次 @168MHz, 够 SCCB 用 */
{
	volatile uint32_t i;

	while (us--)
	{
		i = 80U;
		while (i--) { }
	}
}

static void CAM_Delay_ms(uint32_t ms)
{
	volatile uint32_t i;

	while (ms--)
	{
		i = 40000U;                            /* 约 1ms @168MHz */
		while (i--) { }
	}
}

/* ======================= 帧缓冲 ======================= */
/* 120x160 个 RGB565 = 38400 字节, 放 .bss */
static uint16_t s_frame[CAM_ROT_W * CAM_ROT_H];

/* 摄像头是否在线 —— 开机读一次 PID(0x0A) 判断, 正常是 0x76。
   为什么要有这个标志: 没接摄像头时 SCCB 写不报错, 但抓帧会一路走到
   VSYNC 超时(每帧白等 200ms×3)。上层看到 0 就直接显示"无信号",
   不进抓帧路径。 */
static uint8_t s_cam_ok = 0U;

/* ======================= SCCB ======================= */
/* OV7670 的从机地址: 写 = 0x42, 读 = 0x43 */
#define OV7670_ADDR_W       0x42U
#define OV7670_ADDR_R       0x43U

static void SCCB_Start(void)
{
	SCCB_SDA_H();
	SCCB_SCL_H();
	CAM_Delay_us(2U);
	SCCB_SDA_L();                              /* SCL 高时 SDA 下降 = START */
	CAM_Delay_us(2U);
	SCCB_SCL_L();
	CAM_Delay_us(2U);
}

static void SCCB_Stop(void)
{
	SCCB_SDA_L();
	SCCB_SCL_H();
	CAM_Delay_us(2U);
	SCCB_SDA_H();                              /* SCL 高时 SDA 上升 = STOP */
	CAM_Delay_us(2U);
}

/* 发一个字节(MSB first)。SCCB 的第 9 位是"不必关心位", 主机不管从机怎么应答 */
static void SCCB_SendByte(uint8_t b)
{
	uint8_t i;

	for (i = 0U; i < 8U; i++)
	{
		if ((b & 0x80U) != 0U) { SCCB_SDA_H(); } else { SCCB_SDA_L(); }
		CAM_Delay_us(2U);
		SCCB_SCL_H();
		CAM_Delay_us(2U);
		SCCB_SCL_L();
		CAM_Delay_us(2U);
		b = (uint8_t)(b << 1);
	}

	/* 第 9 位: 主机发"不必关心", 给一个时钟走完 */
	SCCB_SDA_H();
	CAM_Delay_us(2U);
	SCCB_SCL_H();
	CAM_Delay_us(2U);
	SCCB_SCL_L();
	CAM_Delay_us(2U);
}

/* 收一个字节(MSB first)。第 9 位主机发 NAK(SDA 保持高) */
static uint8_t SCCB_RecvByte(void)
{
	uint8_t i, b = 0U;

	SCCB_SDA_H();                              /* 主机释放 SDA, 由从机驱动 */

	for (i = 0U; i < 8U; i++)
	{
		b = (uint8_t)(b << 1);
		CAM_Delay_us(2U);
		SCCB_SCL_H();
		CAM_Delay_us(2U);
		if (SCCB_SDA_READ() != Bit_RESET) { b |= 1U; }
		SCCB_SCL_L();
		CAM_Delay_us(2U);
	}

	/* 第 9 位: 主机发 NAK */
	SCCB_SDA_H();
	CAM_Delay_us(2U);
	SCCB_SCL_H();
	CAM_Delay_us(2U);
	SCCB_SCL_L();
	CAM_Delay_us(2U);

	return b;
}

/* ======================= 寄存器读写 ======================= */
void OV7670_WriteReg(uint8_t reg, uint8_t val)
{
	SCCB_Start();
	SCCB_SendByte(OV7670_ADDR_W);
	SCCB_SendByte(reg);
	SCCB_SendByte(val);
	SCCB_Stop();
}

/* 读寄存器要分两段: 先"写"寄存器号, 再"读"数据(OV7670 的 SCCB 规定) */
uint8_t OV7670_ReadReg(uint8_t reg)
{
	uint8_t v;

	SCCB_Start();
	SCCB_SendByte(OV7670_ADDR_W);
	SCCB_SendByte(reg);
	SCCB_Stop();

	SCCB_Start();
	SCCB_SendByte(OV7670_ADDR_R);
	v = SCCB_RecvByte();
	SCCB_Stop();

	return v;
}

/* ======================= 初始化寄存器表 ======================= */
/* 来源: 资料/OV7670/案例.txt —— 一份**实际跑通过**的 QVGA RGB565 配置,
   作者在每条后面标了"注释掉这条会出什么毛病"。比照着手册自己拼可靠得多,
   所以这里逐条照抄它启用的那些寄存器(它注释掉的就不写, 走芯片默认值)。
   寄存器含义以 资料/OV7670/OV7670中文版数据手册.pdf 表5 为准。

   ⚠ 这份案例是按 **24MHz XCLK** 写的, 本模块晶振是 **12MHz**
     (见 CMOS_FIFO电路.pdf)。对带 FIFO 的模块来说时钟只影响帧率,
     不影响正确性 —— 我们是从 FIFO 里按自己的节奏读的。 */
typedef struct { uint8_t reg; uint8_t val; } ov_reg_t;

static const ov_reg_t OV7670_InitRegs[] =
{
	/* ---- 帧率 / 时钟 ---- */
	{ 0x11, 0x80 },     /* CLKRC: 不分频 */
	{ 0x6B, 0x0A },     /* DBLV: 案例原话"将 PLL 调高的话就会产生花屏", 别动 */
	{ 0x2A, 0x00 },     /* 空行(帧率微调) */
	{ 0x2B, 0x00 },
	{ 0x92, 0x00 },
	{ 0x93, 0x00 },
	{ 0x3B, 0x0A },     /* 条纹滤波器 */

	/* ---- 输出格式 ---- */
	{ 0x12, 0x14 },     /* COM7 : bit4=QVGA 320x240, bit2=RGB */
	{ 0x40, 0x10 },     /* COM15: bit[5:4]=01 -> RGB565(手册表5) */
	{ 0x8C, 0x00 },

	/* ---- 特效: 全关 ---- */
	{ 0x3A, 0x04 },     /* TSLB */
	{ 0x67, 0xC0 },
	{ 0x68, 0x80 },

	/* ---- 镜像/翻转: 0x00 = 都不翻 ----
	   画面若是左右反的或上下颠倒, **先物理转一下摄像头模块**;
	   实在要软件改就改这: bit5=水平镜像, bit4=竖直翻转 */
	{ 0x1E, 0x00 },     /* MVFP */

	/* ---- 输出窗口 ---- */
	{ 0x17, 0x16 },     /* HSTART  ┐ 案例原话: 注释掉这几条会 */
	{ 0x18, 0x04 },     /* HSTOP   │ "倾斜显示, 并显示多块" ——  */
	{ 0x19, 0x02 },     /* VSTART  │ 它们是唯一在控制取景窗口的 */
	{ 0x1A, 0x7B },     /* VSTOP   │ */
	{ 0x32, 0x80 },     /* HREF    │ */
	{ 0x03, 0x06 },     /* VREF    ┘ */

	/* ---- VSYNC 极性 ----
	   手册表5: 0x15 COM10 的 bit1 = "VSYNC 负有效"(低电平有效)。
	   选它有两个理由, 都对得上:
	     · 案例原话"注释这个配置的话, 就显示花屏了"
	     · 我们 CaptureFrame() 等的是 VSYNC 的**下降沿** —— VSYNC 高电平
	       空闲、低电平脉冲时, 下降沿正好是一帧的开头。 */
	{ 0x15, 0x02 },     /* COM10 */

	/* ---- 自动黑电平校正 ----
	   手册把 0xB0 标成 RSVD(保留), 但案例作者实测:
	   "调试时注释这项配置时, 颜色显示不正常了, 红色变绿色, 绿色变红色" */
	{ 0xB0, 0x84 },
};

/* ======================= 初始化 ======================= */
static void OV7670_GpioInit(void)
{
	GPIO_InitTypeDef g;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE | RCC_AHB1Periph_GPIOB, ENABLE);

	/* ---- 数据线 D0~D7 = PE0~PE7: 输入 ---- */
	GPIO_StructInit(&g);
	g.GPIO_Pin  = 0x00FFU;
	g.GPIO_Mode = GPIO_Mode_IN;
	g.GPIO_PuPd = GPIO_PuPd_NOPULL;        /* FIFO 会主动驱动, 不用内部上拉 */
	GPIO_Init(CAM_DATA_PORT, &g);

	/* ---- FIFO 控制 + 摄像头 RESET/PWDN + SCCB_SCL: 推挽输出 ---- */
	GPIO_StructInit(&g);
	g.GPIO_Pin   = FIFO_RCK_PIN | FIFO_OE_PIN | FIFO_RRST_PIN | FIFO_WRST_PIN |
	               FIFO_WR_PIN  | CAM_RST_PIN | CAM_PWDN_PIN | SCCB_SCL_PIN;
	g.GPIO_Mode  = GPIO_Mode_OUT;
	g.GPIO_OType = GPIO_OType_PP;
	g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
	g.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOE, &g);

	/* ---- SCCB_SDA = PB0: 开漏输出(SCCB 是 I2C 的简化版, 读的时候要能放手) ---- */
	GPIO_StructInit(&g);
	g.GPIO_Pin   = SCCB_SDA_PIN;
	g.GPIO_Mode  = GPIO_Mode_OUT;
	g.GPIO_OType = GPIO_OType_OD;
	g.GPIO_PuPd  = GPIO_PuPd_UP;
	g.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(SCCB_SDA_PORT, &g);

	/* ---- VSYNC = PB1: 输入 ---- */
	GPIO_StructInit(&g);
	g.GPIO_Pin  = CAM_VSYNC_PIN;
	g.GPIO_Mode = GPIO_Mode_IN;
	g.GPIO_PuPd = GPIO_PuPd_DOWN;
	GPIO_Init(CAM_VSYNC_PORT, &g);

	/* ---- 初始电平 ---- */
	FIFO_RCK_L();
	FIFO_OE_H();        /* 先不让 FIFO 输出, 免得和别的东西抢总线 */
	FIFO_WR_L();        /* 先不捕获 */
	FIFO_WRST_H();
	FIFO_RRST_H();

	SET_L(CAM_PWDN_PORT, CAM_PWDN_PIN);    /* PWDN 低 = 正常工作(高 = 掉电) */
	SET_H(CAM_RST_PORT,  CAM_RST_PIN);     /* RESET 高 = 正常工作(低 = 复位) */

	SCCB_SCL_H();
	SCCB_SDA_H();
}

void OV7670_Init(void)
{
	uint32_t i;

	OV7670_GpioInit();

	/* ---- 硬件复位: RESET 拉低一下再放开 ---- */
	SET_L(CAM_RST_PORT, CAM_RST_PIN);
	CAM_Delay_ms(20U);
	SET_H(CAM_RST_PORT, CAM_RST_PIN);
	CAM_Delay_ms(50U);

	/* ---- 软复位 ---- */
	OV7670_WriteReg(0x12, 0x80);
	CAM_Delay_ms(100U);

	/* ---- 写配置表 ---- */
	for (i = 0U; i < (sizeof(OV7670_InitRegs) / sizeof(OV7670_InitRegs[0])); i++)
	{
		OV7670_WriteReg(OV7670_InitRegs[i].reg, OV7670_InitRegs[i].val);
	}

	CAM_Delay_ms(100U);

	/* ---- 探测摄像头在不在: 读 PID(0x0A), 正常值是 0x76 ----
	   读得到 = SCCB 通了(接线/供电/上拉都没问题); 读不到就置 0,
	   上层看到就不去抓帧了。 */
	s_cam_ok = (OV7670_ReadReg(0x0A) == 0x76U) ? 1U : 0U;
}

uint8_t OV7670_IsPresent(void)
{
	return s_cam_ok;
}

/* ======================= FIFO 读一个字节 ======================= */
/* 手册: 数据在 RCK 上升沿输出, 同时读指针自增。所以每读一字节发一个上升沿。
 *
 * ⚠⚠ 不要"优化"这个函数(比如把 FIFO_RCK_H/L 改成直接写 BSRR 省掉函数调用)!
 *     AL422B 是 DRAM, 手册要求 WCK 和 RCK 里**较快的那个**充当内部刷新时钟,
 *     而且必须一直跑。本设计里:
 *       WCK = 摄像头的 PCLK = 12MHz, 一直在跑
 *       RCK = 这里的 PE8       ≈ 5~8MHz(受编译器优化等级影响)
 *     RCK 比 WCK 慢, 所以刷新由 WCK 承担 —— 这是安全的, 因为菜单和舵机界面
 *     根本不读 FIFO, RCK 停着也没关系。
 *     一旦把 RCK 提到 12MHz 以上, 刷新就改由 RCK 负责, 而它在我们不读的时候
 *     是停的 —— **FIFO 里的数据会烂掉**, 而且现象很怪(菜单里待久了再进摄像头
 *     界面, 头几帧是花的), 极难往这上面想。
 *     -> 想让读取更快, 只能换 24MHz 晶振的模块, 不要动这里的时序。 */
static inline uint8_t FIFO_ReadByte(void)
{
	uint8_t v;

	FIFO_RCK_H();                              /* 上升沿: 新数据进输出寄存器 */
	v = (uint8_t)(CAM_DATA_PORT->IDR & CAM_DATA_MASK);
	FIFO_RCK_L();

	return v;
}

/* ======================= VSYNC 边沿等待(带超时) ======================= */
/* 等 VSYNC 到达指定电平。返回 1 = 等到了, 0 = 超时。
   ⚠ 超时不是可选项: 摄像头没接/没供电时 VSYNC 被下拉电阻拉成恒低,
      裸 while 会死循环 —— 主循环整个卡死, LED 心跳停、按键全无反应,
      看着就像板子挂了。而且 OV7670_Init() 的 SCCB 写对不存在的从机
      也不报错, 所以一定会走到这里。 */
static uint8_t VSYNC_Wait(BitAction level, uint32_t timeout_ms)
{
	uint32_t t0 = Tick_GetMs();

	while (GPIO_ReadInputDataBit(CAM_VSYNC_PORT, CAM_VSYNC_PIN) != level)
	{
		if (Tick_Elapsed(t0, timeout_ms) != 0U) { return 0U; }
	}
	return 1U;
}

/* ======================= VSYNC 极性诊断 ======================= */
/* 采样 VSYNC, 返回其中读到"高电平"的次数(共 OV7670_VSYNC_SAMPLES 次)。
 *
 * 为什么要这个东西:
 *   抓帧的边沿顺序(先等下降沿=帧首, 再等上升沿=像素开始)完全建立在一个
 *   前提上 —— **VSYNC 平时是高、帧首拉低**, 也就是 COM10(0x15) 的 bit1
 *   "VSYNC 负有效"真的生效了。这一条只能上电实测, 看代码看不出来。
 *   一帧 66.7ms 而同步脉冲很短, 所以只要采样点跨过一帧以上, 平时是高的话
 *   返回值应该接近采样总数。
 *
 * 判读:
 *   返回值 ≈ 总数(比如 9/10)  -> 极性对, 抓帧逻辑没问题
 *   返回值 ≈ 0(比如 0/10)     -> 极性反了! 把 OV7670_CaptureFrame() 里
 *                                前两步的 Bit_RESET / Bit_SET 对调
 *   返回 0 且摄像头也没探测到  -> 摄像头没接好, 先查 SCL/SDA 接线
 */
uint8_t OV7670_VsyncIdleHigh(void)
{
	uint8_t i, n = 0U;

	for (i = 0U; i < OV7670_VSYNC_SAMPLES; i++)
	{
		if (GPIO_ReadInputDataBit(CAM_VSYNC_PORT, CAM_VSYNC_PIN) != Bit_RESET) { n++; }
		CAM_Delay_ms(15U);              /* 拉开间隔, 跨过一帧多才有意义 */
	}
	return n;
}

/* ======================= 抓一帧进 FIFO ======================= */
/* 返回 1 = 抓到完整一帧; 0 = 超时(摄像头没接/没配好)。
 *
 * 为什么改成"由 VSYNC 边沿驱动"而不是"等固定毫秒数":
 *   原来的写法是打开捕获后死等 40ms。但那个 40ms 是拿 CAM_Delay_ms() 数的,
 *   而那个函数是 volatile 自减循环, "约 1ms" 是估的, 实测约 1.4~2.0ms ——
 *   所以 40ms 其实是 55~75ms。偏偏模块晶振是 12MHz, QVGA 一帧要 66.7ms,
 *   正好卡在临界点上: 抓够没抓够全看运气, 表现为画面**间歇性撕裂**。
 *   改成等 VSYNC 边沿之后, 一帧就是一帧, 跟帧率是 15fps 还是 30fps 无关,
 *   也不再依赖那个没标定过的延时函数。 */
uint8_t OV7670_CaptureFrame(void)
{
	/* 1. 先确保不捕获 */
	FIFO_WR_L();

	/* 2. 等 VSYNC 下降沿 = 一帧的开始。
	      COM10(0x15) 的 bit1 配成了"VSYNC 负有效", 所以 VSYNC 平时是高、
	      帧首拉低 —— 下降沿正好是一帧的起点。 */
	if (VSYNC_Wait(Bit_RESET, CAM_VSYNC_TIMEOUT_MS) == 0U) { return 0U; }

	/* 3. 等它回到高 = 同步脉冲结束, 像素开始输出 */
	if (VSYNC_Wait(Bit_SET, CAM_VSYNC_TIMEOUT_MS) == 0U) { return 0U; }

	/* 4. 复位写指针。⚠ 手册要求 WRST 在 WCK 上升沿被采样才生效,
	      而 WCK 是摄像头的 PCLK(12MHz, 83ns 一个周期) —— 这 10us 的脉冲
	      横跨上百个 WCK 周期, 必然被采到。
	      此步 FIFO_WR 还是低的, 所以复位期间不会有数据写进来。 */
	FIFO_WRST_L();
	CAM_Delay_us(10U);
	FIFO_WRST_H();

	/* 5. 打开捕获。模块内部 FIFO_WE = NAND(FIFO_WR, HREF),
	      所以之后只在 HREF 有效(有像素)的期间才真正往 FIFO 里写。 */
	FIFO_WR_H();

	/* 6. 等下一次 VSYNC 下降沿 = 一整帧刚好写完, 立刻停止捕获。
	      超时值给得宽是为了"过等安全": 多写的那些落在第 153600 字节之后,
	      我们根本读不到, 所以宁可多等; 反过来等不够才是致命的(会读到
	      半帧 + 上一帧的残影)。 */
	if (VSYNC_Wait(Bit_RESET, CAM_VSYNC_TIMEOUT_MS) == 0U)
	{
		FIFO_WR_L();
		return 0U;
	}
	FIFO_WR_L();

	return 1U;
}

/* ======================= 读出 + 抽点 + 旋转 ======================= */
void OV7670_ReadFrameRotated(void)
{
	uint16_t sx, sy;

	FIFO_OE_L();                               /* 打开 FIFO 数据输出 */

	/* 复位读指针。⚠ 手册: RRST 要在 RCK 上升沿被采样才生效 ——
	   只把 RRST 拉低再拉高是没用的, 必须伴着一次时钟。这里踩过会很难查。 */
	FIFO_RCK_L();
	FIFO_RRST_L();
	FIFO_RCK_H();                              /* 这个上升沿把 RRST 采进去 */
	FIFO_RCK_L();
	FIFO_RRST_H();

	for (sy = 0U; sy < CAM_H; sy++)
	{
		uint8_t keep_row = (uint8_t)((sy & 1U) == 0U);
		uint16_t oy = (uint16_t)(sy >> 1);

		for (sx = 0U; sx < CAM_W; sx++)
		{
			/* RGB565: 高字节在前, 低字节在后。
			   颜色若整体不对(红蓝互换 / 画面发紫), 十有八九就是这里 ——
			   把两个 FIFO_ReadByte() 对调即可 */
			uint16_t c = (uint16_t)(((uint16_t)FIFO_ReadByte() << 8) |
			                         (uint16_t)FIFO_ReadByte());

			/* 隔行隔列抽点 2:1 -> 320x240 变 160x120 */
			if ((keep_row != 0U) && ((sx & 1U) == 0U))
			{
				uint16_t ox = (uint16_t)(sx >> 1);

				/* 再旋转 90°: 源(ox, oy) -> 目标(119-oy, ox) */
				s_frame[(uint32_t)ox * CAM_ROT_W + (CAM_OUT_H - 1U - oy)] = c;
			}
		}
	}

	FIFO_OE_H();                               /* 关输出, 让出总线 */
}

uint16_t *OV7670_GetFrameBuf(void)
{
	return s_frame;
}
