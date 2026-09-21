/**
  ******************************************************************************
  * @file    SoftWare/inc/Ring_buffer.h
  * @brief   通用无锁环形缓冲区(字节流) —— 典型用途: 串口中断接收
  *
  * ======================= 设计要点 =======================
  * 1. 单生产者 / 单消费者模型下【无锁安全】:
  *      生产者(串口接收中断) 只调用 WriteByte/Write;
  *      消费者(主循环)      只调用 ReadByte/Read;
  *    不需要关中断即可正常工作(Cortex-M 单核, 16 位下标读写是原子的,
  *    且“先写数据、后挪指针”保证了时序)。
  * 2. 通用字节流, 不依赖任何具体串口/芯片外设, 换芯片/串口可直接复用。
  * 3. 用 head/tail 下标 + 取模实现, 刻意预留 1 个空槽位区分“空/满”,
  *    因此【实际最多存放 size-1 个字节】, size 不必是 2 的幂。
  *
  * ================ 与 USART1 中断接收配合示例 ================
  * // 1. 全局实例(定义在 main.c, 需要共享时在其它文件加 extern)
  * RingBuf_t g_uart1_rx;
  * uint8_t   g_uart1_rx_mem[64];
  *
  * // 2. main() 中: 初始化环 + 使能接收中断
  * RingBuf_Init(&g_uart1_rx, g_uart1_rx_mem, sizeof(g_uart1_rx_mem));
  * USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);   // 开接收非空中断
  * NVIC_EnableIRQ(USART1_IRQn);                      // 开 NVIC 通道
  *
  * // 3. 中断服务函数: 来一个字节就塞进环(强符号, 写在 stm32f4xx_it.c)
  * void USART1_IRQHandler(void)
  * {
  *   if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
  *   {
  *     uint8_t ch = (uint8_t)USART_ReceiveData(USART1);  // 读 DR 即清 RXNE
  *     RingBuf_WriteByte(&g_uart1_rx, ch);               // 满则自动丢弃该字节
  *   }
  * }
  *
  * // 4. 主循环里取数据解析
  * uint8_t ch;
  * while (RingBuf_ReadByte(&g_uart1_rx, &ch))
  * {
  *   // 在这里处理每个接收字节: 拼命令 / 回显 / 交给状态机
  * }
  ******************************************************************************
  */

#ifndef __RING_BUFFER_H
#define __RING_BUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ============================ 环形缓冲结构 ============================ */
typedef struct
{
  uint8_t           *buf;   /*!< 数据存储区指针(由调用者提供)  */
  uint16_t           size;  /*!< 存储区容量(字节), 最大可存 size-1 字节 */
  volatile uint16_t  head;  /*!< 写指针: 下一个待写入的下标(由生产者推进) */
  volatile uint16_t  tail;  /*!< 读指针: 下一个待读出的下标(由消费者推进) */
} RingBuf_t;

/* ============================ API ============================ */

/**
  * @brief  初始化环形缓冲(复位为空)
  * @param  rb  : 结构体指针
  * @param  mem : 外部提供的存储区
  * @param  size: 存储区字节数(应 >= 2)
  * @retval true=成功, false=参数非法(mem 为空或 size<2)
  */
bool     RingBuf_Init    (RingBuf_t *rb, uint8_t *mem, uint16_t size);

/**
  * @brief  清空缓冲(无并发时调用; 若与中断并发请先关中断再调用)
  */
void     RingBuf_Reset   (RingBuf_t *rb);

/* ---------------- 查询 ---------------- */
bool     RingBuf_IsEmpty (const RingBuf_t *rb);   /*!< 是否为空 */
bool     RingBuf_IsFull  (const RingBuf_t *rb);   /*!< 是否已满(还剩 1 个保护槽) */
uint16_t RingBuf_Used    (const RingBuf_t *rb);   /*!< 可读字节数 */
uint16_t RingBuf_Free    (const RingBuf_t *rb);   /*!< 还可写字节数 */
bool     RingBuf_Peek    (const RingBuf_t *rb, uint8_t *ch); /*!< 查看队首但不取出 */

/* ---------------- 单字节(生产/消费各一侧, 无锁安全) ---------------- */
bool     RingBuf_WriteByte(RingBuf_t *rb, uint8_t ch);  /*!< 生产者: 写 1 字节 */
bool     RingBuf_ReadByte (RingBuf_t *rb, uint8_t *ch); /*!< 消费者: 读 1 字节 */

/* ---------------- 批量(内部循环调用单字节版本) ---------------- */
uint16_t RingBuf_Write   (RingBuf_t *rb, const uint8_t *src, uint16_t len);
uint16_t RingBuf_Read    (RingBuf_t *rb, uint8_t *dst, uint16_t maxLen);
uint16_t RingBuf_Discard (RingBuf_t *rb, uint16_t cnt);  /*!< 丢弃队首 cnt 字节 */

#ifdef __cplusplus
}
#endif

#endif /* __RING_BUFFER_H */
