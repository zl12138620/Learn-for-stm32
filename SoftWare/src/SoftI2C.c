/**
  ******************************************************************************
  * @file    SoftWare/src/SoftI2C.c
  * @brief   通用软件 I2C 主机实现(bit-bang)
  *
  *          关键点: SCL/SDA 配成「开漏输出 + 上拉」。开漏下写 1 只是释放总线
  *          (靠上拉拉高), 写 0 才是真正拉低; 而输出模式下输入通道依然工作,
  *          所以读 SDA 直接读 IDR 即可, 不必切换输入/输出方向。
  *
  *          时序来源: SSD1306 要求 SCL 高/低电平 >= 0.6us、START/STOP 建立
  *          时间 >= 0.6us, 故每次电平翻转后都插入半周期延时。
  ******************************************************************************
  */

#include "SoftI2C.h"

/* ============================ 内部电平操作 ============================ */
/* 开漏输出: 写 1 = 释放(由外/内上拉拉高), 写 0 = 拉低 */
#define SCL_H(bus)    GPIO_WriteBit((bus)->scl_port, (bus)->scl_pin, Bit_SET)
#define SCL_L(bus)    GPIO_WriteBit((bus)->scl_port, (bus)->scl_pin, Bit_RESET)
#define SDA_H(bus)    GPIO_WriteBit((bus)->sda_port, (bus)->sda_pin, Bit_SET)
#define SDA_L(bus)    GPIO_WriteBit((bus)->sda_port, (bus)->sda_pin, Bit_RESET)

/* 读 SDA 的实际电平(含从机拉低的情况) */
#define SDA_READ(bus) GPIO_ReadInputDataBit((bus)->sda_port, (bus)->sda_pin)

/* 从机地址 + 读写位, 拼成完整的第一字节 */
#define I2C_ADDR_W(bus) ((uint8_t)(((bus)->dev_addr << 1) | 0x00U))
#define I2C_ADDR_R(bus) ((uint8_t)(((bus)->dev_addr << 1) | 0x01U))

/* ============================ 半周期延时 ============================ */
static void I2C_Delay(const SoftI2C_Bus_t *bus)
{
  volatile uint8_t i = (bus->delay != 0U) ? bus->delay : (uint8_t)SOFTI2C_DEFAULT_DELAY;

  while (i--)
  {
  }
}

/* ============================ 初始化 ============================ */
void SoftI2C_Init(const SoftI2C_Bus_t *bus)
{
  GPIO_InitTypeDef gpio;

  RCC_AHB1PeriphClockCmd(bus->scl_rcc, ENABLE);
  if (bus->sda_rcc != bus->scl_rcc)
  {
    RCC_AHB1PeriphClockCmd(bus->sda_rcc, ENABLE);
  }

  GPIO_StructInit(&gpio);
  gpio.GPIO_Mode  = GPIO_Mode_OUT;
  gpio.GPIO_OType = GPIO_OType_OD;   /* 开漏: 只能拉低, 高电平靠上拉 */
  gpio.GPIO_PuPd  = GPIO_PuPd_UP;    /* 内部上拉(弱), 长线请外接 4.7k */
  gpio.GPIO_Speed = GPIO_Speed_50MHz;

  gpio.GPIO_Pin = bus->scl_pin;
  GPIO_Init(bus->scl_port, &gpio);

  gpio.GPIO_Pin = bus->sda_pin;
  GPIO_Init(bus->sda_port, &gpio);

  SCL_H(bus);
  SDA_H(bus);                        /* 空闲态: 两根线都释放为高 */
}

/* ============================ 起止时序 ============================ */
void SoftI2C_Start(const SoftI2C_Bus_t *bus)
{
  SDA_H(bus);
  SCL_H(bus);
  I2C_Delay(bus);
  SDA_L(bus);          /* SCL 为高时拉低 SDA = START */
  I2C_Delay(bus);
  SCL_L(bus);          /* 钳住总线, 准备传数据 */
  I2C_Delay(bus);
}

void SoftI2C_Stop(const SoftI2C_Bus_t *bus)
{
  SDA_L(bus);
  SCL_H(bus);
  I2C_Delay(bus);
  SDA_H(bus);          /* SCL 为高时释放 SDA = STOP */
  I2C_Delay(bus);
}

/* ============================ 收发字节 ============================ */
/* 发送 1 字节(MSB first), 第 9 个时钟读从机应答; 返回 1 = 收到 ACK */
uint8_t SoftI2C_SendByte(const SoftI2C_Bus_t *bus, uint8_t byte)
{
  uint8_t i;
  uint8_t ack;

  for (i = 0U; i < 8U; i++)
  {
    if ((byte & 0x80U) != 0U)
    {
      SDA_H(bus);
    }
    else
    {
      SDA_L(bus);
    }
    byte = (uint8_t)(byte << 1);

    I2C_Delay(bus);
    SCL_H(bus);        /* SCL 高电平期间数据必须稳定 */
    I2C_Delay(bus);
    SCL_L(bus);
    I2C_Delay(bus);
  }

  SDA_H(bus);          /* 第 9 个时钟: 主机释放 SDA, 让从机驱动 */
  I2C_Delay(bus);
  SCL_H(bus);
  I2C_Delay(bus);
  ack = (SDA_READ(bus) == Bit_RESET) ? 1U : 0U;   /* 低电平 = ACK */
  SCL_L(bus);
  I2C_Delay(bus);

  return ack;
}

/* 接收 1 字节(MSB first); ack != 0 时主机回 ACK, 否则回 NACK(读最后一字节用) */
uint8_t SoftI2C_RecvByte(const SoftI2C_Bus_t *bus, uint8_t ack)
{
  uint8_t i;
  uint8_t byte = 0U;

  SDA_H(bus);          /* 主机释放 SDA, 交给从机驱动 */
  for (i = 0U; i < 8U; i++)
  {
    byte = (uint8_t)(byte << 1);

    I2C_Delay(bus);
    SCL_H(bus);
    I2C_Delay(bus);
    if (SDA_READ(bus) == Bit_SET)
    {
      byte |= 0x01U;
    }
    SCL_L(bus);
    I2C_Delay(bus);
  }

  if (ack != 0U)
  {
    SDA_L(bus);        /* 回 ACK: 拉低 */
  }
  else
  {
    SDA_H(bus);        /* 回 NACK: 保持释放 */
  }
  I2C_Delay(bus);
  SCL_H(bus);
  I2C_Delay(bus);
  SCL_L(bus);
  I2C_Delay(bus);
  SDA_H(bus);          /* 释放, 回到空闲 */

  return byte;
}

/* ============================ 探测与寄存器读写 ============================ */
uint8_t SoftI2C_Probe(const SoftI2C_Bus_t *bus)
{
  uint8_t ack;

  SoftI2C_Start(bus);
  ack = SoftI2C_SendByte(bus, I2C_ADDR_W(bus));   /* 写方向探测: 只关心有没有 ACK */
  SoftI2C_Stop(bus);

  return ack;
}

uint8_t SoftI2C_WriteReg(const SoftI2C_Bus_t *bus, uint8_t reg, uint8_t data)
{
  uint8_t ok;

  SoftI2C_Start(bus);
  ok  = SoftI2C_SendByte(bus, I2C_ADDR_W(bus));
  ok &= SoftI2C_SendByte(bus, reg);
  ok &= SoftI2C_SendByte(bus, data);
  SoftI2C_Stop(bus);

  return ok;
}

uint8_t SoftI2C_WriteRegs(const SoftI2C_Bus_t *bus, uint8_t reg, const uint8_t *buf, uint16_t len)
{
  uint8_t  ok;
  uint16_t i;

  SoftI2C_Start(bus);
  ok  = SoftI2C_SendByte(bus, I2C_ADDR_W(bus));
  ok &= SoftI2C_SendByte(bus, reg);
  for (i = 0U; i < len; i++)
  {
    ok &= SoftI2C_SendByte(bus, buf[i]);
  }
  SoftI2C_Stop(bus);

  return ok;
}

uint8_t SoftI2C_ReadReg(const SoftI2C_Bus_t *bus, uint8_t reg, uint8_t *out)
{
  uint8_t ok;

  SoftI2C_Start(bus);
  ok  = SoftI2C_SendByte(bus, I2C_ADDR_W(bus));
  ok &= SoftI2C_SendByte(bus, reg);
  SoftI2C_Start(bus);                              /* 重复 START, 不插入 STOP */
  ok &= SoftI2C_SendByte(bus, I2C_ADDR_R(bus));

  *out = SoftI2C_RecvByte(bus, 0U);                /* 只读 1 字节 -> 回 NACK */
  SoftI2C_Stop(bus);

  return ok;
}

uint8_t SoftI2C_ReadRegs(const SoftI2C_Bus_t *bus, uint8_t reg, uint8_t *buf, uint16_t len)
{
  uint8_t  ok;
  uint16_t i;

  if (len == 0U)
  {
    return 1U;
  }

  SoftI2C_Start(bus);
  ok  = SoftI2C_SendByte(bus, I2C_ADDR_W(bus));
  ok &= SoftI2C_SendByte(bus, reg);
  SoftI2C_Start(bus);
  ok &= SoftI2C_SendByte(bus, I2C_ADDR_R(bus));

  for (i = 0U; i < len; i++)
  {
    /* 倒数第二个字节之后都要回 ACK, 最后一个字节必须回 NACK 让从机放手 */
    buf[i] = SoftI2C_RecvByte(bus, (uint8_t)((i + 1U < len) ? 1U : 0U));
  }
  SoftI2C_Stop(bus);

  return ok;
}

uint8_t SoftI2C_Scan(SoftI2C_Bus_t *bus, uint8_t *found, uint8_t max)
{
  uint8_t addr;
  uint8_t n = 0U;
  uint8_t saved = bus->dev_addr;

  for (addr = 0x08U; addr <= 0x77U; addr++)      /* 7bit 地址的有效区间 */
  {
    bus->dev_addr = addr;
    if (SoftI2C_Probe(bus) != 0U)
    {
      if ((found != 0) && (n < max))
      {
        found[n] = addr;
      }
      n++;
    }
  }

  bus->dev_addr = saved;                          /* 还原调用者的地址 */
  return n;
}
