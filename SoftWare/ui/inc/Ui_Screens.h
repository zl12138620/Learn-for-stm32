/**
  ******************************************************************************
  * @file    SoftWare/ui/inc/Ui_Screens.h
  * @brief   LVGL 版三个界面的对外接口 + 界面调度
  *
  *          ---- 这个模块**不碰任何硬件** ----
  *
  *          它不认识 OV7670 / Servo / Encoder。角度、帧率、画面指针全部由
  *          App.c 当参数传进来, 用户的动作也**回传**给 App.c 去执行。
  *          这条规矩是从老的 display/Menu.h 继承下来的, 那边写得很清楚:
  *            "每个界面都能单独静态画出来验证布局, 不需要摄像头和舵机就位。
  *             出问题时'是画错了'还是'数据错了'一眼就能分开。"
  *
  *          ⚠ 所以本模块**不 include** OV7670.h / Servo.h, 哪怕那是同一层
  *            的兄弟领域、技术上 include 得到。跨领域的胶水一律放 app/。
  *            (Encoder 是个例外 —— LVGL 的输入设备本来就长在移植层里,
  *             见 Lvgl_Port.c。这里不碰它。)
  *
  *          ---- 线程模型 ----
  *          lv_conf.h 里 LV_USE_OS = LV_OS_NONE, **所有函数只能从 UiTask 调**。
  *          CameraTask 只能调 Ui_Camera.h 里明确标注的那两个。
  ******************************************************************************
  */

#ifndef __UI_SCREENS_H
#define __UI_SCREENS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 界面编号 ============================ */
/* ⚠ 顺序要和 App.c 的 ui_screen_t、以及主菜单项目的顺序**一致**。
   三处顺序不一致是这类状态机最经典的 bug, 而且现象是"选舵机进了摄像头"
   这种一看就知道是顺序问题的样子 —— 但排查时容易先怀疑别的地方。 */
typedef enum
{
    UI_SCR_MAIN = 0,        /* 主菜单 */
    UI_SCR_CAMERA,          /* 摄像头 */
    UI_SCR_SERVO            /* 舵机 */
} ui_scr_t;

/* ============================ 用户动作 ============================ */
/* 界面只报告"用户干了什么", 由 App.c 决定"那要做什么"。
   这样 ui 领域保持不碰硬件, 也让界面能在没有舵机/摄像头的板子上单独跑。 */
typedef enum
{
    UI_ACT_ENTER_CAMERA = 0,    /* 主菜单上选中了摄像头 */
    UI_ACT_ENTER_SERVO,         /* 主菜单上选中了舵机 */
    UI_ACT_BACK_MAIN,           /* 在功能界面按了"退出" */
    UI_ACT_SERVO_ANGLE          /* 舵机角度变了, 新角度在 arg 里 */
} ui_action_t;

/* 动作回调。App_Init() 里注册, 一般直接把 App.c 的状态机接上去。 */
typedef void (*ui_action_cb_t)(ui_action_t act, uint8_t arg);

/* ============================ 生命周期 ============================ */

/* 注册动作回调。必须在 UiScreens_Init() 之前调。 */
void UiScreens_SetActionCb(ui_action_cb_t cb);

/* 建三个界面(的控件树)。只在 UiTask 里调一次, App.c 的 LVGL 初始化之后。 */
void UiScreens_Init(void);

/* 给别的 ui 模块用的: 把动作转交给注册的回调。
   (目前只有 Ui_Camera.c 的"按下退出"用到。)
   ⚠ 回调没注册时是空操作, 不会崩 —— 但界面也就没反应了, 所以
     App_Init() 里那行注册不能忘。 */
void UiScreens_Notify(ui_action_t act, uint8_t arg);

/* ============================ 切换界面 ============================
   内部按顺序做四件事, **顺序是有讲究的**:
     ① 切显示(lv_screen_load)
     ② 清空输入分组(lv_group_remove_all_objs)
     ③ 按这个界面的需要设编辑模式(舵机屏要 true, 其余 false)
     ④ 把该界面的控件重新加进分组

   ⚠ 为什么每次切屏都要清空再加: v9.6 的 lv_group_set_default() **只是存个指针**,
     不会让控件自动入组; 而分组里留着上一个界面的控件指针, 就是在给
     "事件送给已删除的控件"埋雷。所以集中在这一个函数里做, 别散在各处。

   ⚠ 编辑模式忘了复位是最容易犯的错: 舵机屏设了 true 之后不设回来,
     回到主菜单转动就变成"改值"而不是"移焦点", 现象是菜单不动。 */
void UiScreens_Enter(ui_scr_t scr);

/* ============================ 数据同步 ============================ */

/* 主菜单当前选中第几项。
   ⚠ 它是由焦点事件维护的, 所以什么时候读都是对的。
     在收到 UI_ACT_ENTER_CAMERA / ENTER_SERVO 之后要读它的话, 读到的就是
     用户当时选中的那一项 —— 这也是为什么两个"进入"动作可以合并成一个。 */
uint8_t UiScreens_GetSel(void);

/* 外部改了舵机角度时把界面同步过去(进舵机屏时推一次当前角度)。
   ⚠ 这个只改**显示**, 不会回调 UI_ACT_SERVO_ANGLE —— 否则 App.c 会收到
     自己刚设进去的值的回声, 形成"设 -> 回调 -> 再设"的循环。 */
void UiScreens_SetAngle(uint8_t deg);

#ifdef __cplusplus
}
#endif

#endif /* __UI_SCREENS_H */
