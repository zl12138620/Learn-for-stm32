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
/* ⚠ 下面每个函数**内部都会获取 LCD 互斥量**, 可以在任意任务里直接调。
   代价是**不能在公开函数之间互相调用** —— 普通互斥量不可重入, 会死锁。
   需要复用就抽不带锁的 static 内部函数(见 Menu.c 的 Menu_CursorDraw /
   Menu_CameraFpsDraw)。 */

/* 建 LCD 互斥量。必须在**建任何会画屏的任务之前**调,
   一般在 App_Init() 里、xTaskCreate 之前。 */
void Menu_Init(void);

/* ---------- 主菜单 ---------- */
void Menu_DrawMain(uint8_t sel);              /* 整屏画一次(进入主菜单时调) */
void Menu_DrawCursor(uint8_t row, uint8_t on);/* 只动"箭头+方框"; 切换选中时调 */

/* ---------- 舵机界面 ---------- */
void Menu_DrawServoChrome(void);              /* 标题/单位/进度条外框(进入时调) */
void Menu_DrawServoValue(uint8_t deg);        /* 角度数字 + 进度条填充(变化时调) */

/* ---------- 摄像头界面 ---------- */
void Menu_DrawCameraChrome(void);             /* 清屏 + 帧率占位(进入时调) */

/* 画面 + 帧率。⚠ **不加锁**, 调用方自己加 —— 而且判断也要在锁里:
       Menu_Lock();
       if (s_screen == UI_CAMERA) { Menu_DrawCameraFrameLocked(buf, fps); }
       Menu_Unlock();
   为什么非要这样, Menu.c 里这个函数的注释写清楚了(界面切换的竞争)。 */
void Menu_DrawCameraFrameLocked(const uint16_t *buf, uint8_t fps);

void Menu_DrawCameraNoSignal(void);           /* 摄像头没接好时的提示 */

/* ---------- 手动加解锁 ----------
   ⚠ 正常情况下**不要用** —— 上面那些 Menu_DrawXxx 自带加锁。
   只有一种场合需要: 判断和画必须原子完成时(见 App.c 的 CameraTask)。 */
void Menu_Lock(void);
void Menu_Unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* __MENU_H */
