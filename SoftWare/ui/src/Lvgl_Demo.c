/**
  ******************************************************************************
  * @file    SoftWare/ui/src/Lvgl_Demo.c
  * @brief   LVGL 演示页 —— 一屏验证"显示 / 输入 / 时基"三条通路
  *
  *          这一页的**唯一目的**是回答一个问题:
  *            "LVGL 在这块 128x160 的小屏上到底能不能看, 能不能接上现有的
  *             旋转编码器?"
  *          这个问题光看文档答不了, 烧上去看一眼最快。
  *
  *          所以它刻意只放**最少**的一组控件, 每一个都对应一条要验证的通路;
  *          控件再少就不足以说明问题, 再多就会因为屏幕太小而挤成一团,
  *          反而看不出 LVGL 正常时是什么样:
  *
  *            标题标签   证明画得出来 —— 最基本的一步
  *            按钮       能被转动选中(焦点)、能被按下触发
  *            勾选框     同上, 换一种控件确认不是碰巧
  *            滑条       转动能改值 —— 这一条直接证明"转"被收到了
  *            时钟       **每秒自己在跳** = LVGL 的定时器 + 时基都通了
  *            命中计数   按一次 +1 = "按"真的送到了控件上
  *
  *          ⚠ 时钟那个标签是这一页里最有价值的一行, 别当装饰:
  *            LVGL 的定时器、动画、长按判定、滚动惯性**全都**建立在它的
  *            tick 上, 而 tick 来自 Tick_GetMs() -> xTaskGetTickCount()。
  *            时钟在跳 = 这整条链是活的。
  *            反过来, 如果**画面出来了但时钟不动**, 那问题一定在时基上
  *            (多半是 Lvgl_Init 在调度器启动之前被调了), 根本不用去怀疑显示。
  *            这就是为什么要放个会自己动的东西 —— 让两种故障一眼可分。
  *
  *          ⚠ 界面文字用英文: LVGL 自带的 montserrat 字体只覆盖 ASCII,
  *            中文要用 lv_font_conv 另外生成字库(本工程的 CN_Font 是给
  *            display 领域那套手写驱动用的, LVGL 不认)。
  *            这一步先不引入字体转换 —— 它是独立的另一件事。
  ******************************************************************************
  */

#include "Lvgl_Port.h"

#include "ui_theme.h"   /* 颜色一律走令牌, 界面代码里不留裸 hex */

#include "lvgl.h"
#include "LCD.h"
#include "Tick.h"

/* ============================ 布局 ============================
   屏幕只有 128x160, 每一行都算着用。左边距 6, 控件宽 116 ——
   留这点边是因为 LVGL 的默认主题会给控件加边框和阴影, 顶到屏幕边缘
   会被裁掉, 看着像是画错了。

   y 坐标是手工排的而不是用 flex 布局: 一共六个控件, 手工排能精确控制
   每一个的位置, 出问题时"哪个控件跑到哪去了"一目了然。
   (真要做复杂界面时该换 flex/grid —— 那本来也是 LVGL 的强项。) */
#define DEMO_X      6
#define DEMO_W      116

#define DEMO_Y_TITLE    2
#define DEMO_Y_BTN      22
#define DEMO_H_BTN      30
#define DEMO_Y_CHECK    58
#define DEMO_Y_SLIDER   88
#define DEMO_H_SLIDER   12
#define DEMO_Y_CLOCK    112
#define DEMO_Y_HITS     134

/* ============================ 演示页状态 ============================ */
/* 这几个只被 UiTask 碰(LVGL 全部调用都在这一个任务里, 见 Lvgl_Port.h),
   所以不需要 volatile, 也不需要加锁。 */
static lv_obj_t *s_clock_lbl = NULL;
static lv_obj_t *s_hits_lbl  = NULL;
static uint32_t  s_hits      = 0U;
static uint32_t  s_t0        = 0U;      /* 演示页开始时的 Tick_GetMs() */

/* ========================================================================
 * 时钟定时器回调: 把"从演示开始到现在过了多久"画成 hh:mm:ss
 *
 *   用的是 Tick_GetMs() 而不是 LVGL 自己记的数 —— 这样它反映的就是
 *   **FreeRTOS 的 tick**, 也就是整个系统真正的时间。
 *   LVGL 的定时器周期写 500ms 而不是 1000ms: 这样秒数变化和刷新之间有
 *   半拍错开, 不会出现"刚好每次都晚一点、看着像卡住"的观感问题。
 *
 *   ⚠ 用 %02d 而不是 %02u: LVGL 自带的那套 sprintf(见 lv_sprintf_builtin.c)
 *     确实支持 %u, 但显示用的这几个数本来就小, 转成 int 写 %d 最省心,
 *     也免得分清 LVGL 的 PRId32/PRIu32 那一套宏。
 * ====================================================================== */
static void Demo_ClockCb(lv_timer_t *t)
{
    uint32_t sec;

    (void)t;

    if (s_clock_lbl == NULL) { return; }

    sec = (Tick_GetMs() - s_t0) / 1000U;

    lv_label_set_text_fmt(s_clock_lbl, "%02d:%02d:%02d",
                          (int)(sec / 3600U),
                          (int)((sec / 60U) % 60U),
                          (int)(sec % 60U));
}

/* ========================================================================
 * 按钮 / 勾选框的命中回调
 *
 *   两个控件共用: 点中按钮是 CLICKED, 勾选框翻转是 VALUE_CHANGED。
 *   不管哪个, 计数 +1 —— 数字动了就说明"按"确实送到了控件上,
 *   而不是被 LVGL 的输入处理吃掉了。
 * ====================================================================== */
static void Demo_HitCb(lv_event_t *e)
{
    (void)e;

    s_hits++;

    if (s_hits_lbl != NULL)
    {
        lv_label_set_text_fmt(s_hits_lbl, "hits: %d", (int)s_hits);
    }
}

/* ========================================================================
 * 建演示页
 *
 *   ⚠ 每次调用都会新建一套控件, 并**再建一个时钟定时器**。
 *     本工程只在开机时调一次。要反复调的话, 得先把旧定时器删掉
 *     (lv_timer_del), 否则定时器会越积越多。
 * ====================================================================== */
void Lvgl_DemoShow(void)
{
    lv_group_t *grp;
    lv_obj_t   *scr;
    lv_obj_t   *o;
    lv_obj_t   *lbl;

    scr = lv_screen_active();

    /* ⚠ 必须先挡 NULL。Lvgl_Init() 里显示设备没建出来时会提前返回,
       此时默认显示是空的, lv_screen_active() 就给 NULL。
       不挡的话下面 lv_label_create(NULL) 之类会去建"屏幕对象", 而它
       内部又要找默认显示 —— 大概率直接崩在 LVGL 里面。 */
    if (scr == NULL)
    {
        Lvgl_DiagMsg("screen is NULL -- 没有可用屏幕, 演示页跳过");
        return;
    }

    /* 清掉上一位占着屏幕的东西。演示结束后 UI_Enter(UI_MAIN_MENU) 那边
       会 LCD_Clear 整屏, 所以这里只是保证演示页自己是从白纸开始的。 */
    lv_obj_clean(scr);

    /* ⚠ 关掉屏幕的滚动。默认是开着的 —— 手工摆的坐标万一有几个像素超出,
       整个页面会变成可滚动的, 现象是"转动编码器画面在上下跑"而不是在
       控件之间移焦点, 很费解。小屏上手工布局时把它关掉是标准做法。
       ⚠ 用 lv_obj_set_scrollable 而不是通用的 lv_obj_remove_flag(scr,
         LV_OBJ_FLAG_SCROLLABLE): 后者在 v9.6 已经标了 @deprecated,
         编译会甩一条 -Wdeprecated-declarations 警告。 */
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, UI_SURFACE_OFF, LV_PART_MAIN);

    /* 焦点分组由 Lvgl_Init() 建好并设成默认分组。
       ⚠ v9.6 里 lv_group_set_default() **只是把一个指针存起来**,
         **不会**让之后新建的控件自动加入 —— 必须逐个 lv_group_add_obj。
         (这个和某些教程说的不一样, 是以源码为准。) */
    grp = lv_group_get_default();

    s_hits = 0U;
    s_t0   = Tick_GetMs();

    /* ---------- 标题 ----------
       2026-09-22 临时改成中文: 这一步是**单独验证字库**(见 工具/gen_lvgl_font.py)。
       ⚠ 故意汉字和 ASCII 混排 —— 顺便验证两件事:
           1. 汉字能不能出来(出不来会显示成占位方块, 不是崩)
           2. ASCII 换成同一套字库之后, 基线和汉字对不对得齐
              (下面的时钟 00:00:00 全是 ASCII, 一起看)
       字库验收完之后把这里改回 "LVGL v9.6.0" 或直接删掉演示页。 */
    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "主菜单 舵机 OK");
    lv_obj_set_pos(lbl, DEMO_X, DEMO_Y_TITLE);

    /* ---------- 按钮 ---------- */
    o = lv_button_create(scr);
    lv_obj_set_pos(o, DEMO_X, DEMO_Y_BTN);
    lv_obj_set_size(o, DEMO_W, DEMO_H_BTN);
    lv_obj_add_event_cb(o, Demo_HitCb, LV_EVENT_CLICKED, NULL);

    lbl = lv_label_create(o);
    lv_label_set_text(lbl, "Button");
    lv_obj_center(lbl);

    if (grp != NULL) { lv_group_add_obj(grp, o); }

    /* 一进来就把焦点放在按钮上。不然屏幕上没有任何东西是"选中"状态,
       要转一下才有反应, 容易被误判成"转动没生效"。 */
    if (grp != NULL) { lv_group_focus_obj(o); }

    /* ---------- 勾选框 ---------- */
    o = lv_checkbox_create(scr);
    lv_checkbox_set_text(o, "Check");
    lv_obj_set_pos(o, DEMO_X, DEMO_Y_CHECK);
    lv_obj_add_event_cb(o, Demo_HitCb, LV_EVENT_VALUE_CHANGED, NULL);

    if (grp != NULL) { lv_group_add_obj(grp, o); }

    /* ---------- 滑条 ---------- */
    o = lv_slider_create(scr);
    lv_obj_set_pos(o, DEMO_X, DEMO_Y_SLIDER);
    lv_obj_set_size(o, DEMO_W, DEMO_H_SLIDER);
    lv_slider_set_range(o, 0, 100);
    lv_slider_set_value(o, 40, LV_ANIM_OFF);

    if (grp != NULL) { lv_group_add_obj(grp, o); }

    /* ---------- 时钟(验证时基) ---------- */
    s_clock_lbl = lv_label_create(scr);
    lv_label_set_text(s_clock_lbl, "--:--:--");
    lv_obj_set_pos(s_clock_lbl, DEMO_X, DEMO_Y_CLOCK);

    /* ---------- 命中计数(验证"按") ---------- */
    s_hits_lbl = lv_label_create(scr);
    lv_label_set_text(s_hits_lbl, "hits: 0");
    lv_obj_set_pos(s_hits_lbl, DEMO_X, DEMO_Y_HITS);

    /* ---------- 时钟定时器 ---------- */
    (void)lv_timer_create(Demo_ClockCb, 500U, NULL);

    /* 控件全建完了, 报一下内存用量: 和 Lvgl_Init() 里那两条对比,
       就能看出"主题"和"控件"各吃掉多少。 */
    Lvgl_DiagReport("demo built");
}
