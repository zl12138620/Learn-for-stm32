/**
  ******************************************************************************
  * @file    SoftWare/inc/SoftI2C.h
  * @brief   通用软件 I2C 主机(bit-bang) —— 引脚由调用者提供, 模块不占用任何固定引脚
  *
  *          特性:
  *            - 完整 START / 重复START / STOP / ACK / NACK 时序
  *            - 7bit 从机地址; SCL 与 SDA 可挂在不同的 GPIO 端口
  *            - 开漏输出 + 上拉, 输出模式下 IDR 仍可读 -> 收发无需来回切换方向
  *            - 纯阻塞实现, 不占用任何定时器/中断
  *
  *          用法示例(以 SSD1306 为例, PB7=SCL / PB8=SDA):
  *            static const SoftI2C_Bus_t s_oled = {
  *              .scl_port = GPIOB, .scl_pin = GPIO_Pin_7, .scl_rcc = RCC_AHB1Periph_GPIOB,
  *              .sda_port = GPIOB, .sda_pin = GPIO_Pin_8, .sda_rcc = RCC_AHB1Periph_GPIOB,
  *              .dev_addr = 0x3C,   // SSD1306 的 7bit 地址(写方向即 0x78)
  *              .delay    = 0,      // 0 = 用默认值 SOFTI2C_DEFAULT_DELAY
  *            };
  *
  *            SoftI2C_Init(&s_oled);
  *            if (SoftI2C_WriteReg(&s_oled, 0x00, 0xAF) == 0) {
  *              // 从机没应答: 接线/上拉/地址 三样里挑一样查
  *            }
  *
  *          注意:
  *            - 模块内部上拉很弱, 长线或高速请外接 4.7k 上拉
  *            - 全部接口阻塞执行, 不要在中断服务函数里调用
  *            - 无总线仲裁/时钟拉伸处理, 只适合单主机、单/多从机的简单场景
  ******************************************************************************
  */

#ifndef __SOFTI2C_H
#define __SOFTI2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* 未显式指定 delay 时的半周期延时循环次数(约 1us @168MHz, 与 OLED.c 取值一致)
   SCL/SDA 时序要求: 高/低电平 >= 0.6us, START/STOP 建立时间 >= 0.6us。
   屏幕仍不稳定就加大, 刷新太慢就减小。 */
#define SOFTI2C_DEFAULT_DELAY   40U

/* ============================ 总线描述 ============================ */
typedef struct
{
  GPIO_TypeDef *scl_port;   /* SCL 所在端口, 如 GPIOB */
  GPIO_TypeDef *sda_port;   /* SDA 所在端口, 如 GPIOB */
  uint32_t      scl_rcc;    /* SCL 端口时钟, 如 RCC_AHB1Periph_GPIOB */
  uint32_t      sda_rcc;    /* SDA 端口时钟, 如 RCC_AHB1Periph_GPIOB */
  uint16_t      scl_pin;    /* SCL 引脚, 如 GPIO_Pin_7 */
  uint16_t      sda_pin;    /* SDA 引脚, 如 GPIO_Pin_8 */
  uint8_t       dev_addr;   /* 7bit 从机地址(不含 R/W 位), 如 SSD1306 = 0x3C */
  uint8_t       delay;      /* 半周期延时循环次数, 0 = SOFTI2C_DEFAULT_DELAY */
} SoftI2C_Bus_t;

/* ============================ API ============================ */
/* 返回值约定: 1 = 成功/收到 ACK, 0 = 失败/收到 NACK */

void    SoftI2C_Init(const SoftI2C_Bus_t *bus);      /* 配置 SCL/SDA 为开漏输出并使总线空闲 */

void    SoftI2C_Start(const SoftI2C_Bus_t *bus);     /* START: SCL 高时 SDA 由高变低 */
void    SoftI2C_Stop(const SoftI2C_Bus_t *bus);      /* STOP : SCL 高时 SDA 由低变高 */

uint8_t SoftI2C_SendByte(const SoftI2C_Bus_t *bus, uint8_t byte);   /* MSB first, 返回从机 ACK */
uint8_t SoftI2C_RecvByte(const SoftI2C_Bus_t *bus, uint8_t ack);    /* MSB first, ack!=0 则主机回 ACK */

uint8_t SoftI2C_Probe(const SoftI2C_Bus_t *bus);     /* 探测当前 dev_addr 是否有器件应答 */

/* 寄存器读写: 先发 从机地址+写 / 寄存器号, 再收或发数据 */
uint8_t SoftI2C_WriteReg(const SoftI2C_Bus_t *bus, uint8_t reg, uint8_t data);
uint8_t SoftI2C_WriteRegs(const SoftI2C_Bus_t *bus, uint8_t reg, const uint8_t *buf, uint16_t len);
uint8_t SoftI2C_ReadReg(const SoftI2C_Bus_t *bus, uint8_t reg, uint8_t *out);
uint8_t SoftI2C_ReadRegs(const SoftI2C_Bus_t *bus, uint8_t reg, uint8_t *buf, uint16_t len);

/* 扫描整条总线(7bit 地址 0x08~0x77), 命中的地址写入 found[], 返回命中个数。
   bus 非 const: 扫描期间会临时改动 dev_addr, 结束时还原。 */
uint8_t SoftI2C_Scan(SoftI2C_Bus_t *bus, uint8_t *found, uint8_t max);

#ifdef __cplusplus
}
#endif

#endif /* __SOFTI2C_H */
