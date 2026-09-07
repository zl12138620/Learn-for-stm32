/**
  ******************************************************************************
  * @file    SoftWare/src/Ring_buffer.c
  * @brief   通用无锁环形缓冲区实现(详见 Ring_buffer.h 头部说明)
  *
  *          空/满判定(预留 1 个保护槽, 最大可存 size-1 字节):
  *            空: head == tail
  *            满: (head + 1) % size == tail
  *
  *          无锁安全性(单生产者/单消费者):
  *            - 写侧只改 head: 先把数据写入 buf[head], 最后才推进 head;
  *            - 读侧只改 tail: 先取走 buf[tail], 最后才推进 tail;
  *            两个指针各自只被一方修改、另一方只读, 不会产生竞争条件,
  *            因此串口接收中断里直接调用即可, 无需进入临界区。
  ******************************************************************************
  */

#include "Ring_buffer.h"
#include <stddef.h>        /* NULL */

/* ========================================================================
 * 内部工具: 已用字节数 / 剩余可写字节数
 * ====================================================================== */
static uint16_t RB_Used(const RingBuf_t *rb)
{
  return (uint16_t)((uint16_t)(rb->head - rb->tail + rb->size) % rb->size);
}

static uint16_t RB_Free(const RingBuf_t *rb)
{
  /* 总容量 size, 预留 1 槽用于区分空/满 */
  return (uint16_t)(rb->size - 1U - RB_Used(rb));
}

/* ========================================================================
 * 初始化 / 复位
 * ====================================================================== */
bool RingBuf_Init(RingBuf_t *rb, uint8_t *mem, uint16_t size)
{
  if ((rb == NULL) || (mem == NULL) || (size < 2U))
  {
    return false;               /* 参数非法 */
  }

  rb->buf  = mem;
  rb->size = size;
  rb->head = 0U;
  rb->tail = 0U;
  return true;
}

void RingBuf_Reset(RingBuf_t *rb)
{
  if (rb == NULL)
  {
    return;
  }
  /* 注意: 若与接收中断并发调用(例如运行中清缓冲), 请先临时关闭该中断 */
  rb->head = 0U;
  rb->tail = 0U;
}

/* ========================================================================
 * 查询接口
 * ====================================================================== */
bool RingBuf_IsEmpty(const RingBuf_t *rb)
{
  return (rb == NULL) || (rb->head == rb->tail);
}

bool RingBuf_IsFull(const RingBuf_t *rb)
{
  if (rb == NULL)
  {
    return true;
  }
  return (uint16_t)((rb->head + 1U) % rb->size) == rb->tail;
}

uint16_t RingBuf_Used(const RingBuf_t *rb)
{
  return (rb == NULL) ? 0U : RB_Used(rb);
}

uint16_t RingBuf_Free(const RingBuf_t *rb)
{
  return (rb == NULL) ? 0U : RB_Free(rb);
}

bool RingBuf_Peek(const RingBuf_t *rb, uint8_t *ch)
{
  if ((rb == NULL) || (ch == NULL) || (rb->head == rb->tail))
  {
    return false;               /* 空缓冲 */
  }
  *ch = rb->buf[rb->tail];      /* 只查看, 不移除 */
  return true;
}

/* ========================================================================
 * 单字节写入(生产者侧: 串口接收中断中调用)
 * 说明: 写满时返回 false 并丢弃新字节——绝不会覆盖尚未读取的旧数据,
 *       这是环形缓冲与 FIFO 相比最核心的容错行为。
 * ====================================================================== */
bool RingBuf_WriteByte(RingBuf_t *rb, uint8_t ch)
{
  uint16_t next;

  if (rb == NULL)
  {
    return false;
  }

  next = (uint16_t)((rb->head + 1U) % rb->size);
  if (next == rb->tail)
  {
    return false;               /* 满: 丢弃 */
  }

  rb->buf[rb->head] = ch;       /* 先写数据... */
  rb->head = next;              /* ...最后推进写指针(保证读侧永远看不到半新数据) */
  return true;
}

/* ========================================================================
 * 单字节读取(消费者侧: 主循环中调用)
 * ====================================================================== */
bool RingBuf_ReadByte(RingBuf_t *rb, uint8_t *ch)
{
  if ((rb == NULL) || (ch == NULL) || (rb->head == rb->tail))
  {
    return false;               /* 空缓冲 */
  }

  *ch = rb->buf[rb->tail];      /* 先取数据... */
  rb->tail = (uint16_t)((rb->tail + 1U) % rb->size);   /* ...再推进读指针 */
  return true;
}

/* ========================================================================
 * 批量写入(生产者侧)
 * ====================================================================== */
uint16_t RingBuf_Write(RingBuf_t *rb, const uint8_t *src, uint16_t len)
{
  uint16_t i;

  if ((rb == NULL) || (src == NULL))
  {
    return 0U;
  }

  for (i = 0U; i < len; i++)
  {
    if (!RingBuf_WriteByte(rb, src[i]))
    {
      break;                    /* 缓冲满, 写不下即停 */
    }
  }
  return i;                     /* 实际写入字节数 */
}

/* ========================================================================
 * 批量读取(消费者侧)
 * ====================================================================== */
uint16_t RingBuf_Read(RingBuf_t *rb, uint8_t *dst, uint16_t maxLen)
{
  uint16_t i;

  if ((rb == NULL) || (dst == NULL))
  {
    return 0U;
  }

  for (i = 0U; i < maxLen; i++)
  {
    if (!RingBuf_ReadByte(rb, &dst[i]))
    {
      break;                    /* 读空即停 */
    }
  }
  return i;                     /* 实际读出字节数 */
}

/* ========================================================================
 * 丢弃队首 cnt 字节(例如超时后放弃一组半截数据)
 * ====================================================================== */
uint16_t RingBuf_Discard(RingBuf_t *rb, uint16_t cnt)
{
  uint16_t used;
  uint16_t i;

  if (rb == NULL)
  {
    return 0U;
  }

  used = RB_Used(rb);
  if (cnt > used)
  {
    cnt = used;                 /* 最多只能丢弃全部现存数据 */
  }
  for (i = 0U; i < cnt; i++)
  {
    rb->tail = (uint16_t)((rb->tail + 1U) % rb->size);
  }
  return cnt;                   /* 实际丢弃字节数 */
}
