/**
  ******************************************************************************
  * @file    USER/main.h
  * @note    本副本源自 ST 官方模板:
  *          Project/STM32F4xx_StdPeriph_Templates/main.h (V1.8.1, 27-January-2022)
  * @brief   Header for USER/main.c module
  *          （若上游模板更新，请同步此文件）
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx.h"
#include <stdbool.h>

#define BUFFER_SIZE 256


typedef struct
{
  uint8_t buffer[BUFFER_SIZE];
  uint8_t head;
  uint8_t tail;
  /* data */
}RXbuffer;


/**
 * @brief 接收环形缓冲区
 */
//缓冲区初始化
void RXbuffer_Init(RXbuffer *cb)
{
  cb->head = 0;
  cb->tail = 0;
}

//判断缓冲区是否已满
bool BUFFER_IS_Full(const RXbuffer *cb)
{
  return (cb->head+1) % BUFFER_SIZE == cb->tail; 
}

//判断缓冲区是否为空
bool BUFFER_IS_Empty(const RXbuffer *cb)
{
  return cb->head == cb->tail;
}

//缓冲区写入数据字节
bool BUFFER_Write(RXbuffer *cb, uint8_t data)
{
  if(BUFFER_IS_Full(cb)){return false;}
  
  cb->buffer[cb->head] = data;
  cb->head = (cb->head + 1) % BUFFER_SIZE;
  return true;
}

//缓存区读取字节
bool BUFFER_Read(RXbuffer *cb, uint8_t *data)
{
  if(BUFFER_IS_Empty(cb)){return false;}
  *data = cb->buffer[cb->tail];
  cb->tail = (cb->tail+1) % BUFFER_SIZE;
  return true;
}

//缓存区大小读取
uint16_t BUFFER_SIZE_checkout(const RXbuffer *cb)
{
  if(cb->head >= cb->tail){return cb->head - cb->tail;}
  else{return BUFFER_SIZE - (cb->tail - cb->head - 1);}
}

//缓冲区读取数据
bool BUFFER_READDATA(RXbuffer *cb, uint8_t *data, uint16_t length) 
{
  if (BUFFER_SIZE_checkout(cb) < length) {
    return false;  // 缓冲区中的数据不足
  }
  
  for (uint16_t i = 0; i < length; ++i) {
    if (!BUFFER_Read(cb, &data[i])) {
      return false;  // 读取数据出错
    }
  }
  return true; 
} 

//缓冲区写入数据
bool BUFFER_Writedata(RXbuffer *cb, uint8_t *data, uint16_t lenth)
{
  if(BUFFER_SIZE_checkout(cb) < lenth){return false;}
  for(uint16_t i=0; i<lenth; ++i)
  {
    if(!BUFFER_Write(cb, data[i])){return false;} //写入字节失败
  }
  return true;
}












#endif /* __MAIN_H */
