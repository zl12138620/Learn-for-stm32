/**
  ******************************************************************************
  * @file    SoftWare/inc/Menu.h
  * @brief   TFT 功能菜单的三个界面(主菜单 / 舵机 / 摄像头)的绘制
  *
  *          ⚠ 本模块**只管画, 不管逻辑, 也不碰任何硬件**。
  *            它不 include OV7670.h / Servo.h / Encoder.h, 不持有界面状态,
  *            不调用摄像头或舵机 —— 要显示的数据(角度、帧率)全部由 main.c
  *            当参数传进来。
  *
  *            这么分是有意的: 每个界面都能单独静态画出来验证布局,
  *            不需要摄像头和舵机就位。出问题时"是画错了"还是"数据错了"
  *            一眼就能分开。
  *
  *          状态机(在哪个界面、选中第几项)在 USER/main.c 里。
  *
  *          字号: 汉字 16x16, ASCII 8x16。屏幕 128x160, (0,0) 是用户看到的左上角。
  ******************************************************************************
  */

#ifndef __MENU_H
#define __MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================ 菜单项 ============================ */
/* 加新功能时: 在这里加一项, 再在 Menu.c 的 s_items[] 里加对应的名字,
   最后在 main.c 的按键处理里加一个 case —— 三处改完就成。 */
typedef enum
{
    MENU_ITEM_CAMERA = 0,       /* OV7670 摄像头(带 FIFO) */
    MENU_ITEM_SERVO,            /* SG90 舵机 */
    MENU_ITEM_COUNT
} menu_item_t;

/* ============================ API ============================ */
/* ---------- 主菜单 ---------- */
void Menu_DrawMain(uint8_t sel);              /* 整屏画一次(进入主菜单时调) */
void Menu_DrawCursor(uint8_t row, uint8_t on);/* 只动"箭头+方框"; 切换选中时调 */

/* ---------- 舵机界面 ---------- */
void Menu_DrawServoChrome(void);              /* 标题/单位/进度条外框(进入时调) */
void Menu_DrawServoValue(uint8_t deg);        /* 角度数字 + 进度条填充(变化时调) */

/* ---------- 摄像头界面 ---------- */
void Menu_DrawCameraChrome(void);             /* 右上角"帧率"标签(进入时调) */
void Menu_DrawCameraFps(uint8_t fps);         /* 每帧调, 紧跟 LCD_DrawImage 之后 */
void Menu_DrawCameraNoSignal(void);           /* 摄像头没接好时的提示 */

#ifdef __cplusplus
}
#endif

#endif /* __MENU_H */
