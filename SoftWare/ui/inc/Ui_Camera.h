/**
  ******************************************************************************
  * @file    SoftWare/ui/inc/Ui_Camera.h
  * @brief   摄像头界面 —— 把实时的 OV7670 画面做成一个 LVGL 控件
  *
  *          这是整个 LVGL 改造里**最需要小心的一块**, 单独成一个模块。
  *
  *          ---- 和以前有什么不一样 ----
  *
  *          以前(手写 Menu.c 那版): 摄像头抓完帧,**绕过** LVGL/菜单,
  *          直接 Menu_Lock() + LCD_DrawImage() 把整块画面推上屏。
  *          屏幕因此有**两个主人** —— 菜单任务和摄像头任务。
  *          为了不让两边互相盖掉, 专门搞了一套"判断必须在锁里做"的纪律
  *          (见 Menu.c 里 Menu_DrawCameraFrameLocked 的长注释)。
  *
  *          现在: 画面就是 LVGL 的一个 lv_image 控件。**屏幕归 LVGL 一个人管**,
  *          那套锁的纪律整个不需要了。代价是多一次内存拷贝(每帧约 38KB,
  *          10fps 下约 384KB/s, 对 168MHz 的 M4 可以忽略)。
  *
  *          ---- 线程模型(这块必须理解, 不然会写出难查的 bug) ----
  *
  *          lv_conf.h 里 LV_USE_OS = LV_OS_NONE, 也就是 **LVGL 没有任何内部锁,
  *          所有 lv_xxx() 必须在同一个任务里调**(本工程 = UiTask)。
  *          所以 CameraTask **一个 lv_* 都不能碰**, 它只做两件事:
  *
  *            1. 往帧缓冲 s_frame 里填数据(纯内存写, 另一个任务只读)
   *            2. 把序号往前推一格( UiCam_MarkDirty )
  *
  *          UiTask 发现序号变了才去 invalidate + 渲染。
  *
  *          ⚠ **栅栏的方向容易搞反**: UiTask 渲染那 15ms 里正在从 s_frame
  *            里 memcpy, 这时候如果 CameraTask 开始抓下一帧、把缓冲覆写了,
  *            画面就会**上半帧旧、下半帧新**(撕裂)。
  *            所以栅栏要挡住的是**抓帧那头**: CameraTask 在开始抓帧前
  *            必须等 UiCam_IsRendering() 变回 0。
  ******************************************************************************
  */

#ifndef __UI_CAMERA_H
#define __UI_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 画面尺寸 ============================ */
/* 120x160 —— 摄像头输出(转正之后)就是这个尺寸, 1:1 贴到屏上, 不缩放。
   ⚠ 屏幕是 128 宽, 所以左右各空 4 列, 那 4 列由屏幕背景色填。
     位置常量写在这里而不是让调用方传 —— 它是"画在哪"的决定,
     和摄像头的输出尺寸是两件事。 */
#define UICAM_W         120U
#define UICAM_H         160U
#define UICAM_X           4U
#define UICAM_Y           0U

/* ============================ 生命周期 ============================
   ⚠ 全部只能从 **UiTask** 调用(LV_OS_NONE, 见文件头)。 */

/* 建摄像头界面(画面控件 + 帧率 + 无信号提示 + 退出用的透明按钮)。
   返回建好的屏幕对象, 交给 Ui_Screens 挂到显示上。

   buf 是**帧缓冲**的地址, 允许传 NULL(表示"摄像头没探到")。
   传 NULL 时画面控件不建, 只显示"无信号"。
   ⚠ buf 的生命周期必须覆盖整个界面存活期 —— 本工程传的是
     OV7670.c 里的 static 数组, 满足。 */
void UiCam_Build(const uint16_t *buf);

/* 销毁本模块建的所有控件。切走时调, 免得 LVGL 的池子里越积越多。 */
void UiCam_Destroy(void);

/* 返回本界面的根对象(给 Ui_Screens 挂显示用)。没建过返回 NULL。 */
void *UiCam_Root(void);

/* 把本界面的可聚焦控件加进默认分组。
   ⚠ 由 UiScreens_Enter 在**每次切进来时**调 —— 因为那个函数会先清空分组。
     摄像头界面的成员是那个全屏透明按钮, 没有它"按下退出"不成立。 */
void UiCam_GroupAttach(void);

/* ============================ 帧更新 ============================ */

/* 有新帧了。**只做一件事: 把序号往前推。**
   ⚠ CameraTask 可以调(LVGL 对这个操作无感知, 它只写一个 volatile 变量)。
   ⚠ 真正的 invalidate 在 UiTask 里, 由 UiCam_Pump() 做。 */
void UiCam_MarkDirty(void);

/* UiTask 每轮调一次: 如果序号变了就 invalidate + 渲染一帧。
   ⚠ 内部会同步等渲染和送屏完成(本工程的 flush 回调是阻塞的),
     所以返回时画面一定已经上屏, 栅栏窗口是准的。 */
void UiCam_Pump(void);

/* ============================ 帧缓冲栅栏 ============================ */

/* UiTask 渲染期间 = 1。**CameraTask 在开始抓下一帧之前必须等它变回 0**,
   否则会撕裂画面(见文件头)。
   ⚠ 语义要看清: 它栅的是"帧缓冲", 不是"屏幕" —— 屏幕已经不需要栅了。 */
uint8_t UiCam_IsRendering(void);

/* ============================ 帧率 ============================ */
/* 由 App.c 的帧率统计传进来(CameraTask 那边数, UiTask 这边画)。
   只在值真的变了的时候才动 LVGL —— 每秒最多一次。 */
void UiCam_SetFps(uint8_t fps);

#ifdef __cplusplus
}
#endif

#endif /* __UI_CAMERA_H */
