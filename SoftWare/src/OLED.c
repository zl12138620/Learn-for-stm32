#include "OLED_Font.h"
#include "OLED.h"


/* ======================= 引脚 / 时序配置 ======================= */
#define OLED_I2C_PORT       GPIOB
#define OLED_SCL_PIN        GPIO_Pin_8          /* SCL = PB8 */
#define OLED_SDA_PIN        GPIO_Pin_9          /* SDA = PB9 */

#define OLED_SLAVE_ADDR     0x78                /* SSD1315 7bit 地址 0x3C, 左移一位得 0x78 */

/* 半周期延时, 单位是空转循环次数(约 1us @168MHz), 运行时可调。
   这个值不是随便定的 —— 它由**上拉电阻的强弱**决定:
     模块自带 4.7k 上拉 -> 上升时间约 0.5us, 半周期 1us 就够(默认值 40)
     模块没有上拉电阻   -> 只剩 STM32 内部约 40k 的弱上拉, 上升时间
                           约 3.4us(40k x 100pF); 这时半周期必须 > 4us,
                           否则时钟还没升到高电平就翻回去了, 从机会完全
                           看不到时钟 -> 表现就是"发了半天没人应答"。
   拿不准就调 OLED_SetSpeed() 逐档试, 见 main.c 里的扫描。 */
#define OLED_I2C_DELAY_DEFAULT  40U

static volatile uint32_t s_i2c_delay = OLED_I2C_DELAY_DEFAULT;

static void OLED_I2C_Delay(void)
{
	volatile uint32_t i = s_i2c_delay;
	while (i--) { }
}

/* 设置 I2C 半周期延时(空转循环次数), 值越大越慢 */
void OLED_SetSpeed(uint32_t loops)
{
	if (loops == 0U) { loops = 1U; }
	s_i2c_delay = loops;
}

/* 毫秒级粗延时。计数器加了 volatile: 原来那种
   "for(i=0;i<1000;i++) for(j=0;j<1000;j++);" 的空循环在 -O2 下
   有被整个优化掉的风险, 那上电延时就等于没生效。 */
static void OLED_Delay_ms(uint32_t ms)
{
	while (ms--)
	{
		volatile uint32_t i = 40000;    /* 约 1ms @168MHz */
		while (i--) { }
	}
}

/*引脚配置
  开漏输出: 只能主动拉低, 高电平交给上拉电阻 —— I2C 必须这样接。
  顺带的好处是输出模式下 IDR 依然反映引脚真实电平, 所以读应答
  不用把引脚切成输入, 省掉来回切方向的麻烦。 */
#define OLED_W_SCL(x)   do { GPIO_WriteBit(OLED_I2C_PORT, OLED_SCL_PIN, (BitAction)(x)); OLED_I2C_Delay(); } while (0)
#define OLED_W_SDA(x)   do { GPIO_WriteBit(OLED_I2C_PORT, OLED_SDA_PIN, (BitAction)(x)); OLED_I2C_Delay(); } while (0)

/*引脚初始化*/
void OLED_I2C_Init(void)
{
    // RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
	GPIO_InitTypeDef GPIO_InitStructure;
 	// GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_StructInit(&GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_OD;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
	GPIO_InitStructure.GPIO_Pin = OLED_SCL_PIN | OLED_SDA_PIN;
	/* 内部上拉(约 40k)是保险: 模块自带 4.7k 时两者并联, 外部强得多不影响;
	   万一模块没焊上拉电阻, 这一步就是"能不能亮"的分水岭 */
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
 	GPIO_Init(GPIOB, &GPIO_InitStructure);
	OLED_W_SCL(1);
	OLED_W_SDA(1);
}

/**
  * @brief  I2C开始
  * @param  无
  * @retval 无
  */
void OLED_I2C_Start(void)
{
	OLED_W_SDA(1);
	OLED_W_SCL(1);
	OLED_W_SDA(0);
	OLED_W_SCL(0);
}

/**
  * @brief  I2C停止
  * @param  无
  * @retval 无
  */
void OLED_I2C_Stop(void)
{
	OLED_W_SDA(0);
	OLED_W_SCL(1);
	OLED_W_SDA(1);
}

/**
  * @brief  I2C发送一个字节
  * @param  Byte 要发送的一个字节
  * @retval 无
  */
void OLED_I2C_SendByte(uint8_t Byte)
{
	uint8_t i;
	for (i = 0; i < 8; i++)
	{
		OLED_W_SDA(!!(Byte & (0x80 >> i)));
		OLED_W_SCL(1);
		OLED_W_SCL(0);
	}
}

/* 读从机的应答位(第 9 个时钟)。
   先把 SDA 释放(写 1, 靠上拉拉高), 再给一个时钟, 期间读引脚:
     SDA 被从机拉低 -> 应答, 返回 1
     SDA 一直是高   -> 没人应答, 返回 0
   开漏输出模式下 IDR 仍反映真实电平, 所以不用把引脚切成输入。 */
uint8_t OLED_I2C_ReadAck(void)
{
	uint8_t ack;

	OLED_W_SDA(1);                       /* 释放 SDA */
	OLED_W_SCL(1);
	ack = (GPIO_ReadInputDataBit(OLED_I2C_PORT, OLED_SDA_PIN) == Bit_RESET) ? 1U : 0U;
	OLED_W_SCL(0);

	return ack;
}

/* ========================================================================
 * 总线体检与恢复
 * ====================================================================== */

/* 放开两根线, 读回真实电平。
   开漏输出"写 1"等于放手, 此时 IDR 读到的是上拉电阻(或从机)决定的真实电平:
     读回 0 -> 有东西把它拉住不放(从机卡住, 或线短到地) */
uint8_t OLED_BusCheck(void)
{
	uint8_t v = 0U;

	OLED_W_SDA(1);
	OLED_W_SCL(1);

	if (GPIO_ReadInputDataBit(OLED_I2C_PORT, OLED_SCL_PIN) == Bit_RESET) { v |= 0x01U; }
	if (GPIO_ReadInputDataBit(OLED_I2C_PORT, OLED_SDA_PIN) == Bit_RESET) { v |= 0x02U; }

	return v;
}

/* 总线卡死恢复。
   从机正在发数据时如果 MCU 突然复位(重新烧录、按复位键都会),
   从机会卡在"正在发某一位"的状态里, 把 SDA 拉着不放 ——
   之后主机再怎么发都失败。这就是 I2C 最经典的"总线被拖死"。
   标准解法: 主机只发时钟不放数据, 最多补 9 个脉冲, 让从机把这一位走完;
   末尾再补一个 STOP, 把总线上所有从机的状态机复位。 */
uint8_t OLED_BusRecover(void)
{
	uint8_t i;

	if ((OLED_BusCheck() & 0x02U) == 0U)
	{
		return 0U;                  /* SDA 已经是高 —— 总线空闲, 不用管 */
	}

	OLED_W_SDA(1);                  /* 主机放手, 只发时钟 */
	for (i = 0U; i < 9U; i++)
	{
		OLED_W_SCL(0);
		OLED_W_SCL(1);
		if (GPIO_ReadInputDataBit(OLED_I2C_PORT, OLED_SDA_PIN) != Bit_RESET)
		{
			break;                  /* 从机松手了, 立刻停 */
		}
	}

	/* 补一个 STOP: SCL 保持高, SDA 由低变高 */
	OLED_W_SDA(0);
	OLED_W_SCL(1);
	OLED_W_SDA(1);

	return OLED_BusCheck();         /* 恢复后到底通没通, 让调用者自己判断 */
}

/* 探测屏在不在: 只发从机地址, 看有没有应答。
   全黑时先调它 —— 有应答说明接线/供电/上拉都没问题, 该去查驱动代码;
   没应答就是硬件侧, 先量线。 */
uint8_t OLED_Probe(void)
{
	uint8_t ack;

	OLED_I2C_Start();
	OLED_I2C_SendByte(OLED_SLAVE_ADDR);
	ack = OLED_I2C_ReadAck();
	OLED_I2C_Stop();

	return ack;
}

/**
  * @brief  OLED写命令
  * @param  Command 要写入的命令
  * @retval 无
  */
void OLED_WriteCommand(uint8_t Command)
{
	OLED_I2C_Start();
	OLED_I2C_SendByte(OLED_SLAVE_ADDR);		//从机地址
	OLED_I2C_SendByte(0x00);				//写命令(控制字节: Co=0, D/C#=0)
	OLED_I2C_SendByte(Command);
	OLED_I2C_ReadAck();						//补上第 9 个时钟收应答, 时序才完整
	OLED_I2C_Stop();
}

/**
  * @brief  OLED写数据
  * @param  Data 要写入的数据
  * @retval 无
  */
void OLED_WriteData(uint8_t Data)
{
	OLED_I2C_Start();
	OLED_I2C_SendByte(OLED_SLAVE_ADDR);		//从机地址
	OLED_I2C_SendByte(0x40);				//写数据(控制字节: Co=0, D/C#=1)
	OLED_I2C_SendByte(Data);
	OLED_I2C_ReadAck();						//补上第 9 个时钟收应答, 时序才完整
	OLED_I2C_Stop();
}

/**
  * @brief  OLED设置光标位置
  * @param  Y 以左上角为原点，向下方向的坐标，范围：0~7
  * @param  X 以左上角为原点，向右方向的坐标，范围：0~127
  * @retval 无
  */
void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	OLED_WriteCommand(0xB0 | Y);					//设置Y位置
	OLED_WriteCommand(0x10 | ((X & 0xF0) >> 4));	//设置X位置高4位
	OLED_WriteCommand(0x00 | (X & 0x0F));			//设置X位置低4位
}

/**
  * @brief  OLED清屏
  * @param  无
  * @retval 无
  */
void OLED_Clear(void)
{  
	uint8_t i, j;
	for (j = 0; j < 8; j++)
	{
		OLED_SetCursor(j, 0);
		for(i = 0; i < 128; i++)
		{
			OLED_WriteData(0x00);
		}
	}
}

/**
  * @brief  OLED显示一个字符
  * @param  Line 行位置，范围：1~4
  * @param  Column 列位置，范围：1~16
  * @param  Char 要显示的一个字符，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)
{      	
	uint8_t i;
	OLED_SetCursor((Line - 1) * 2, (Column - 1) * 8);		//设置光标位置在上半部分
	for (i = 0; i < 8; i++)
	{
		OLED_WriteData(OLED_F8x16[Char - ' '][i]);			//显示上半部分内容
	}
	OLED_SetCursor((Line - 1) * 2 + 1, (Column - 1) * 8);	//设置光标位置在下半部分
	for (i = 0; i < 8; i++)
	{
		OLED_WriteData(OLED_F8x16[Char - ' '][i + 8]);		//显示下半部分内容
	}
}

/**
  * @brief  OLED显示字符串
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  String 要显示的字符串，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String)
{
	uint8_t i;
	for (i = 0; String[i] != '\0'; i++)
	{
		OLED_ShowChar(Line, Column + i, String[i]);
	}
}

/**
  * @brief  OLED次方函数
  * @retval 返回值等于X的Y次方
  */
uint32_t OLED_Pow(uint32_t X, uint32_t Y)
{
	uint32_t Result = 1;
	while (Y--)
	{
		Result *= X;
	}
	return Result;
}

/**
  * @brief  OLED显示数字（十进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~4294967295
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i;
	for (i = 0; i < Length; i++)							
	{
		OLED_ShowChar(Line, Column + i, Number / OLED_Pow(10, Length - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED显示数字（十进制，带符号数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：-2147483648~2147483647
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length)
{
	uint8_t i;
	uint32_t Number1;
	if (Number >= 0)
	{
		OLED_ShowChar(Line, Column, '+');
		Number1 = Number;
	}
	else
	{
		OLED_ShowChar(Line, Column, '-');
		Number1 = -Number;
	}
	for (i = 0; i < Length; i++)							
	{
		OLED_ShowChar(Line, Column + i + 1, Number1 / OLED_Pow(10, Length - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED显示数字（十六进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~0xFFFFFFFF
  * @param  Length 要显示数字的长度，范围：1~8
  * @retval 无
  */
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i, SingleNumber;
	for (i = 0; i < Length; i++)							
	{
		SingleNumber = Number / OLED_Pow(16, Length - i - 1) % 16;
		if (SingleNumber < 10)
		{
			OLED_ShowChar(Line, Column + i, SingleNumber + '0');
		}
		else
		{
			OLED_ShowChar(Line, Column + i, SingleNumber - 10 + 'A');
		}
	}
}

/**
  * @brief  OLED显示数字（二进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~1111 1111 1111 1111
  * @param  Length 要显示数字的长度，范围：1~16
  * @retval 无
  */
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i;
	for (i = 0; i < Length; i++)							
	{
		OLED_ShowChar(Line, Column + i, Number / OLED_Pow(2, Length - i - 1) % 2 + '0');
	}
}

/**
  * @brief  OLED初始化
  * @param  无
  * @retval 无
  */
void OLED_Init(void)
{
	/* 上电延时: SSD1315 要求 VDD 稳定后至少等 20ms 才能开始发命令,
	   这里给 100ms 留足余量 */
	OLED_Delay_ms(100);

	OLED_I2C_Init();			//端口初始化

	OLED_WriteCommand(0xAE);	//关闭显示

	/* 显式设成页寻址模式。上电默认就是它, 但写出来更保险 ——
	   下面 OLED_SetCursor 用的是 0xB0/0x10/0x00 那套页寻址命令,
	   万一别处把它改成水平/垂直寻址, 光标就会乱跑 */
	OLED_WriteCommand(0x20);
	OLED_WriteCommand(0x02);
	
	OLED_WriteCommand(0xD5);	//设置显示时钟分频比/振荡器频率
	OLED_WriteCommand(0x80);
	
	OLED_WriteCommand(0xA8);	//设置多路复用率
	OLED_WriteCommand(0x3F);
	
	OLED_WriteCommand(0xD3);	//设置显示偏移
	OLED_WriteCommand(0x00);
	
	OLED_WriteCommand(0x40);	//设置显示开始行
	
	OLED_WriteCommand(0xA1);	//设置左右方向，0xA1正常 0xA0左右反置
	
	OLED_WriteCommand(0xC8);	//设置上下方向，0xC8正常 0xC0上下反置

	OLED_WriteCommand(0xDA);	//设置COM引脚硬件配置
	OLED_WriteCommand(0x12);
	
	OLED_WriteCommand(0x81);	//设置对比度控制
	OLED_WriteCommand(0xCF);

	OLED_WriteCommand(0xD9);	//设置预充电周期
	OLED_WriteCommand(0xF1);

	OLED_WriteCommand(0xDB);	//设置VCOMH取消选择级别
	OLED_WriteCommand(0x30);

	OLED_WriteCommand(0xA4);	//设置整个显示打开/关闭

	OLED_WriteCommand(0xA6);	//设置正常/倒转显示

	OLED_WriteCommand(0x8D);	//设置充电泵
	OLED_WriteCommand(0x14);

	OLED_WriteCommand(0xAF);	//开启显示
		
	OLED_Clear();				//OLED清屏
}
