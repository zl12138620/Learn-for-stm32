/**
  ******************************************************************************
  * @file    SoftWare/ui/src/Ui_Screens.c
  * @brief   LVGL 版主菜单 / 舵机界面 + 界面调度 —— 见 Ui_Screens.h
  *
  *          视觉按 lvgl-ui skill 的规矩重做过, 效果图见 docs/UI设计稿-v1.png。
  *          和它逐条对应的实现要点:
  *
  *            · 颜色/圆角/间距**只从 ui_theme.h 取**, 这个文件里没有一个
  *              裸的 lv_color_hex(...) —— skill 的头号反模式就是
  *              "hex 颜色散落在屏幕代码里"。
  *            · 行样式**建一次, 所有行共用**(下面的 st_row_*), 不逐个调样式。
  *            · **只有选中行是卡片**, 未选中行不套框 —— skill 明确点名
  *              "Every control boxed in its own card" 是反模式。
  *              所以层次来自"只有它有实体", 不是来自字号。
  *            · 真实状态: FOCUS(选中) / PRESSED(按下) 都有。
  *
  *          ⚠ 事件回调就写在这个文件里, 和它操作的控件放在一起 ——
  *            和本工程"中断和它读写的变量放同一个文件"是同一条理由。
  ******************************************************************************
  */

#include "Ui_Screens.h"
#include "Ui_Camera.h"
#include "ui_theme.h"

#include "lvgl.h"

/* ============================ 布局 ============================ */
/* 全部落在 8 像素网格上(见 ui_theme.h 第 3 节)。 */
#define ROW_STEP        (UI_ROW_H + UI_GAP)     /* 两行之间的步进 = 48 */
#define L_RULE_W        (128 - 2 * UI_MARGIN)   /* 分隔线宽度 = 112 */

#define HINT1_Y         122
#define HINT2_Y         140

/* 行内部(相对行左上角) */
#define ROW_BAR_X       3       /* 强调条 */
#define ROW_BAR_W       3
#define ROW_BAR_Y       8
#define ROW_BAR_H       (UI_ROW_H - 16)
#define ROW_TEXT_X      10      /* 文字左边, 给强调条让位 */
#define ROW_NAME_DY     3
#define ROW_SUB_DY      22

/* 舵机屏 */
#define S_LABEL_DY      36      /* "角度" */
#define S_VALUE_DY      28      /* 大数值 —— 比标签高, 靠字号撑起来 */
#define S_BAR_Y         68
#define S_BAR_H         16
#define S_SCALE_DY      90

/* ============================ 菜单项 ============================ */
/* ⚠ 顺序必须和 Ui_Screens.h 的 ui_scr_t、App.c 的 ui_screen_t 一致 */
#define MENU_ITEM_COUNT  2U

static const char *const s_item_name[MENU_ITEM_COUNT] = { "摄像头", "舵机" };
static const char *const s_item_sub [MENU_ITEM_COUNT] = { "实时画面", "旋转调角度" };

/* ============================ 共享样式 ============================ */
/* ⚠ 建一次, 所有行共用。skill 的规矩: "Every button is built by the same
   helper and gets the same anatomy and states. Never hand-style buttons
   individually." 样式集中在这里, 加一个状态只改这一处。

   加样式的**顺序**有讲究: 后加的覆盖先加的, 所以顺序是
   基础 -> 选中 -> 按下。 */
static lv_style_t st_row;           /* 基础: 透明、无框、直角容器 */
static lv_style_t st_row_focus;     /* 选中: 实心卡片 + 强调色描边 */
static lv_style_t st_row_pressed;   /* 按下: 更亮的表面 + 下沉 1px */

static void row_styles_init(void)
{
    lv_style_init(&st_row);
    lv_style_set_bg_opa(&st_row, LV_OPA_TRANSP);       /* 未选中: 没有实体 */
    lv_style_set_border_width(&st_row, 0);
    lv_style_set_radius(&st_row, UI_RADIUS);
    lv_style_set_shadow_width(&st_row, 0);             /* 小屏不开阴影 */
    lv_style_set_pad_all(&st_row, 0);                  /* 子控件自己定位 */

    /* 选中 —— 这一屏的层次全靠它 */
    lv_style_init(&st_row_focus);
    lv_style_set_bg_color(&st_row_focus, UI_SURFACE);
    lv_style_set_bg_opa(&st_row_focus, LV_OPA_COVER);
    lv_style_set_border_width(&st_row_focus, 1);
    lv_style_set_border_color(&st_row_focus, UI_ACCENT);

    /* 按下 —— 编码器按下时也给一点反馈(不是只有触摸设备才需要) */
    lv_style_init(&st_row_pressed);
    lv_style_set_bg_color(&st_row_pressed, UI_SURFACE_PRESSED);
    lv_style_set_bg_opa(&st_row_pressed, LV_OPA_COVER);
    lv_style_set_translate_y(&st_row_pressed, 1);
}

/* ============================ 状态 ============================ */
/* ⚠ 只有 UiTask 碰, 所以不需要 volatile / 锁 */
static uint8_t  s_sel   = 0U;       /* 主菜单当前选中项(由焦点事件维护) */
static uint8_t  s_angle = 90U;      /* 舵机当前角度(上电回中位) */

static ui_action_cb_t s_action_cb = NULL;

/* 主菜单 */
static lv_obj_t *s_main_root = NULL;
static lv_obj_t *s_row_btn[MENU_ITEM_COUNT];

/* 舵机 */
static lv_obj_t *s_servo_root = NULL;
static lv_obj_t *s_servo_bar  = NULL;
static lv_obj_t *s_servo_num  = NULL;

/* ========================================================================
 * 小工具
 * ====================================================================== */

/* 建一块纯色矩形(分隔线 / 强调条) */
static lv_obj_t *Ui_MakeRect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);

    return o;
}

/* 建一个标签 */
static lv_obj_t *Ui_MakeLabel(lv_obj_t *parent, const char *text,
                             lv_coord_t x, lv_coord_t y, lv_color_t fg)
{
    lv_obj_t *o = lv_label_create(parent);

    lv_obj_remove_style_all(o);     /* 主题对 label 本来就没样式, 显式更放心 */
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_text_color(o, fg, LV_PART_MAIN);

    return o;
}

/* 标题 + 它下面那条分隔线。两个屏共用。
   ⚠ 标题**故意用和正文一样的 16px**, 不放大 —— 见 ui_theme.h 里的说明:
     这一屏的主角是内容, 标题只是抬头, 放大它就是把层次搞反。
     老界面那条件蓝色的实心标题栏也去掉了, 一条细线足够分区。 */
static void Ui_MakeTitle(lv_obj_t *parent, const char *title)
{
    (void)Ui_MakeLabel(parent, title, UI_MARGIN, 2, UI_TEXT);
    (void)Ui_MakeRect(parent, UI_MARGIN, UI_RULE_Y, L_RULE_W, 1, UI_BORDER);
}

/* 底部提示两行 */
static void Ui_MakeHints(lv_obj_t *parent, const char *h1, const char *h2)
{
    lv_obj_t *lbl;

    lbl = Ui_MakeLabel(parent, h1, 0, HINT1_Y, UI_TEXT_DIM);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, HINT1_Y);

    lbl = Ui_MakeLabel(parent, h2, 0, HINT2_Y, UI_TEXT_DIM);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, HINT2_Y);
}

/* ========================================================================
 * 事件回调
 * ====================================================================== */

/* 主菜单某一行被按下 */
static void Ui_RowClickCb(lv_event_t *e)
{
    uintptr_t item = (uintptr_t)lv_event_get_user_data(e);

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) { return; }

    if (item == 0U) { UiScreens_Notify(UI_ACT_ENTER_CAMERA, 0U); }
    else            { UiScreens_Notify(UI_ACT_ENTER_SERVO,  0U); }
}

/* 主菜单某一行获得 / 失去焦点。
   只干一件事: 记住"现在选中第几项" —— App.c 在按下时要读它决定进哪一屏。
   ⚠ 视觉上的变化(卡片、强调条)**不在这里做**, 靠样式和状态传播自动完成,
     见 row_styles_init 和 Ui_BuildMain 里 lv_obj_set_state_trickle 的说明。 */
static void Ui_RowFocusCb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint8_t   i;

    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) { return; }

    /* ⚠ 让焦点事件来维护 s_sel, 而不是按下的那一刻再去问"焦点是谁":
       这样只有一个真相来源, 也不会出现"焦点动过了但 s_sel 还是旧的"。 */
    for (i = 0U; i < MENU_ITEM_COUNT; i++)
    {
        if (s_row_btn[i] == btn) { s_sel = i; return; }
    }
}

/* 舵机界面: 转动。
   ⚠ 这个回调收到的是 lv_bar **自己没有处理**的 LV_EVENT_KEY ——
     lv_bar 内部完全没有 LV_KEY_* 的处理, 所以我们是唯一的处理者,
     不用担心被类自带的逻辑盖掉。
     (这也正是这里用 lv_bar 而不用 lv_slider 的原因: slider 的类处理函数
      会先把左右键吃掉、按 ±1 走一格, 而且它跑在用户回调**之前**, 拦不住。
      我们需要 5 度一档, 还必须是可控的。) */
static void Ui_ServoKeyCb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_RIGHT)
    {
        s_angle = (uint8_t)((s_angle >= 175U) ? 180U : (s_angle + 5U));
    }
    else if (key == LV_KEY_LEFT)
    {
        s_angle = (uint8_t)((s_angle <= 5U) ? 0U : (s_angle - 5U));
    }
    else
    {
        return;             /* LV_KEY_ENTER 之类一律不管 */
    }

    /* 界面上立即反映 */
    if (s_servo_bar != NULL)
    {
        lv_bar_set_value(s_servo_bar, (int32_t)s_angle, LV_ANIM_OFF);
    }
    if (s_servo_num != NULL)
    {
        /* ⚠ 定宽三位右对齐。不定宽的话 100 变 99 会少一位, 留下上一次的
           半个数字。老代码的 Menu_DrawNum 就是为这个存在的。 */
        lv_label_set_text_fmt(s_servo_num, "%3d", (int)s_angle);
    }

    /* 真正的动作交给 App.c(ui 领域不碰硬件) */
    UiScreens_Notify(UI_ACT_SERVO_ANGLE, s_angle);
}

/* 舵机界面: 按下退出 */
static void Ui_ServoExitCb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        UiScreens_Notify(UI_ACT_BACK_MAIN, 0U);
    }
}

/* ========================================================================
 * 主菜单
 * ====================================================================== */
static void Ui_BuildMain(void)
{
    uint8_t i;
    lv_obj_t *btn;
    lv_obj_t *bar;
    lv_group_t *g;

    s_main_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_main_root);
    lv_obj_set_scrollable(s_main_root, false);
    lv_obj_set_style_bg_color(s_main_root, UI_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_main_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_main_root, 0, LV_PART_MAIN);

    Ui_MakeTitle(s_main_root, "主菜单");

    for (i = 0U; i < MENU_ITEM_COUNT; i++)
    {
        lv_coord_t y = (lv_coord_t)(UI_ROW0_Y + (uint32_t)i * ROW_STEP);

        /* ---- 行 ---- */
        btn = lv_button_create(s_main_root);
        lv_obj_remove_style_all(btn);       /* 先扔掉主题那套蓝底/圆角/投影 */
        lv_obj_set_pos(btn, UI_MARGIN, y);
        lv_obj_set_size(btn, L_RULE_W, UI_ROW_H);
        lv_obj_set_scroll_on_focus(btn, false);

        lv_obj_add_style(btn, &st_row, 0);
        lv_obj_add_style(btn, &st_row_focus, LV_STATE_FOCUS_KEY);
        lv_obj_add_style(btn, &st_row_pressed, LV_STATE_PRESSED);

        /* ⚠⚠ 这一行是"强调条能自动跟随焦点"的关键, 少了两处的任何一处都不亮:
             · LV_OBJ_FLAG_STATE_TRICKLE 默认是**关的**(lv_obj_add_state 里
               有 `if(lv_obj_is_state_trickle(obj))` 才往下传),
               所以必须显式打开, 子控件才会收到 FOCUS_KEY;
             · 强调条自己的样式要对 FOCUS_KEY 设 bg_opa(见下)。
           老版本的界面里"选中时名字变黄"就是这么失效的 —— 给子标签设了
           FOCUS_KEY 的文字色, 但它永远收不到那个状态。 */
        lv_obj_set_state_trickle(btn, true);

        lv_obj_add_event_cb(btn, Ui_RowClickCb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, Ui_RowFocusCb, LV_EVENT_FOCUSED, NULL);

        /* 左侧强调条: 未选中时透明, 选中时亮起强调色。
           ⚠ 它靠**状态传播**自动切换, 不需要事件回调 —— 前提是上面那行
             lv_obj_set_state_trickle。 */
        bar = Ui_MakeRect(btn, ROW_BAR_X, ROW_BAR_Y, ROW_BAR_W, ROW_BAR_H, UI_ACCENT);
        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_FOCUS_KEY);

        /* 名字 + 说明。
           ⚠ 名字在两种状态下**都是 UI_TEXT**, 不跟着焦点变色 ——
             选中的标志是"卡片 + 强调条", 不需要再叠一层颜色变化。
             skill: "Colour = meaning. If a colour does not encode state or
             category, remove it." 已经有个更清楚的信号了, 就不加第二个。 */
        (void)Ui_MakeLabel(btn, s_item_name[i], ROW_TEXT_X, ROW_NAME_DY, UI_TEXT);
        (void)Ui_MakeLabel(btn, s_item_sub[i], ROW_TEXT_X, ROW_SUB_DY, UI_TEXT_DIM);

        s_row_btn[i] = btn;
    }

    Ui_MakeHints(s_main_root, "旋转选择", "按下进入");

    /* 分组: 两个按钮。⚠ 逐个 add —— v9.6 的 lv_group_set_default() 只是存指针,
       不会自动收控件。 */
    g = lv_group_get_default();
    if (g != NULL)
    {
        for (i = 0U; i < MENU_ITEM_COUNT; i++)
        {
            lv_group_add_obj(g, s_row_btn[i]);
        }
    }
}

/* ========================================================================
 * 舵机界面
 * ====================================================================== */
static void Ui_BuildServo(void)
{
    lv_obj_t *bar;
    lv_obj_t *lbl;

    s_servo_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_servo_root);
    lv_obj_set_scrollable(s_servo_root, false);
    lv_obj_set_style_bg_color(s_servo_root, UI_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_servo_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_servo_root, 0, LV_PART_MAIN);

    Ui_MakeTitle(s_servo_root, "舵机");

    (void)Ui_MakeLabel(s_servo_root, "角度", UI_MARGIN, S_LABEL_DY, UI_TEXT_DIM);

    /* ---- 角度数值: 这一屏唯一的主角 ----
       ⚠ 它是整个界面里**唯一**放大的元素(28px 数字字库)。
          skill: "One primary element per screen (the thing the screen is
          about). Make it bigger or more central." 舵机屏说的就是这个数。 */
    s_servo_num = lv_label_create(s_servo_root);
    lv_obj_remove_style_all(s_servo_num);
    lv_label_set_text(s_servo_num, " 90");          /* 定宽三位, 见 Ui_ServoKeyCb */
    lv_obj_set_style_text_font(s_servo_num, UI_FONT_NUM, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_servo_num, UI_TEXT, LV_PART_MAIN);
    lv_obj_align(s_servo_num, LV_ALIGN_TOP_RIGHT, -UI_MARGIN, S_VALUE_DY);

    /* ---- 进度条 ----
       ⚠ 一个 lv_bar 就把老代码的"画外框 + 填绿条"两件事都包了:
         外框 = PART_MAIN 的 border, 填充 = PART_INDICATOR。
       ⚠ **pad_all = 2 是关键**: 指示器画在 MAIN 的**内容区**上(扣掉内边距
         之后那块), 所以 pad 2 正好复现老代码的 S_BAR_PAD = 2。
       ⚠ 主题默认给 lv_bar 挂的是圆角胶囊 + 蓝色 muted 底, 下面逐项掰回来:
         轨道用 UI_SURFACE(比背景亮一档 = "这里是槽"), 填充用 UI_ACCENT
         (和选中态同一个强调色 = "当前值")。 */
    bar = lv_bar_create(s_servo_root);
    lv_obj_set_pos(bar, UI_MARGIN, S_BAR_Y);
    lv_obj_set_size(bar, L_RULE_W, S_BAR_H);
    lv_bar_set_range(bar, 0, 180);
    lv_bar_set_value(bar, (int32_t)s_angle, LV_ANIM_OFF);

    lv_obj_set_style_radius(bar, UI_RADIUS_SM, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, UI_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, UI_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bar, 0, LV_PART_MAIN);

    lv_obj_set_style_radius(bar, UI_RADIUS_SM - 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);

    /* 编辑模式下主题会给 lv_bar 挂一圈 outline("现在可以转了")。
       关掉 —— 焦点已经由别的方式表达了, 这一屏就一根滑条, 不需要再提示。 */
    lv_obj_set_style_outline_width(bar, 0, LV_STATE_EDITED);
    lv_obj_set_scroll_on_focus(bar, false);

    lv_obj_add_event_cb(bar, Ui_ServoKeyCb,  LV_EVENT_KEY,     NULL);
    lv_obj_add_event_cb(bar, Ui_ServoExitCb, LV_EVENT_CLICKED, NULL);

    s_servo_bar = bar;

    /* 刻度: 轨道两端, 用 dim 色 —— 它们是刻度不是内容 */
    (void)Ui_MakeLabel(s_servo_root, "0", UI_MARGIN, S_SCALE_DY, UI_TEXT_DIM);

    lbl = Ui_MakeLabel(s_servo_root, "180", 0, S_SCALE_DY, UI_TEXT_DIM);
    lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -UI_MARGIN, S_SCALE_DY);

    Ui_MakeHints(s_servo_root, "旋转调角", "按下退出");
}

/* ========================================================================
 * 对外接口
 * ====================================================================== */
void UiScreens_SetActionCb(ui_action_cb_t cb)
{
    s_action_cb = cb;
}

void UiScreens_Notify(ui_action_t act, uint8_t arg)
{
    if (s_action_cb != NULL)
    {
        s_action_cb(act, arg);
    }
}

void UiScreens_Init(void)
{
    /* ⚠ 共享样式必须**在建任何控件之前**初始化 —— 建行的时候就要用它们。 */
    row_styles_init();

    /* ⚠ 主菜单要先建 —— 它是 UiScreens_Enter(UI_SCR_MAIN) 的落脚点,
       而且 Lvgl_Init 里那个默认分组已经建好了, 建控件时就能入组。 */
    Ui_BuildMain();
    Ui_BuildServo();
}

void UiScreens_Enter(ui_scr_t scr)
{
    lv_group_t *g = lv_group_get_default();

    /* ① 清空分组。
       ⚠ 必须在切屏时做: 分组里留着上一个界面的控件指针, 就是在给
         "事件送给已删除/已隐藏的控件"埋雷。集中在这一个地方做。 */
    if (g != NULL)
    {
        lv_group_remove_all_objs(g);
        /* ② 复位编辑模式。⚠ 这一步不能漏: 舵机界面把它设成 true(转动=改值),
             不复位的话回到主菜单转动就变成"改值"而不是"移焦点",
             现象是菜单完全不动。 */
        lv_group_set_editing(g, false);
    }

    /* ③ 切显示 + ④ 按界面挂控件 */
    switch (scr)
    {
        case UI_SCR_CAMERA:
            if (UiCam_Root() != NULL)
            {
                lv_screen_load((lv_obj_t *)UiCam_Root());
                UiCam_GroupAttach();
            }
            break;

        case UI_SCR_SERVO:
            if (s_servo_root != NULL)
            {
                lv_screen_load(s_servo_root);
                if (g != NULL)
                {
                    lv_group_add_obj(g, s_servo_bar);
                    lv_group_focus_obj(s_servo_bar);
                    /* ⚠ 编辑模式 = 转动直接改值, 而不是移焦点。
                       整个界面只有一根滑条, 没有别的地方可移, 所以必须开。
                       (lv_indev 的编码器分支: editing 时 enc_diff 被翻译成
                        LV_KEY_LEFT/RIGHT 发给焦点控件, 见 lv_indev.c) */
                    lv_group_set_editing(g, true);
                }
            }
            break;

        case UI_SCR_MAIN:
        default:
            if (s_main_root != NULL)
            {
                lv_screen_load(s_main_root);
                if (g != NULL)
                {
                    uint8_t i;
                    for (i = 0U; i < MENU_ITEM_COUNT; i++)
                    {
                        lv_group_add_obj(g, s_row_btn[i]);
                    }
                    /* 恢复上次选中的那一项 */
                    if (s_sel < MENU_ITEM_COUNT)
                    {
                        lv_group_focus_obj(s_row_btn[s_sel]);
                    }
                }
            }
            break;
    }
}

/* 外部(App.c)改角度时的同步口 —— 比如进界面时把当前角度推上来。 */
void UiScreens_SetAngle(uint8_t deg)
{
    if (deg > 180U) { deg = 180U; }

    s_angle = deg;

    if (s_servo_bar != NULL)
    {
        lv_bar_set_value(s_servo_bar, (int32_t)s_angle, LV_ANIM_OFF);
    }
    if (s_servo_num != NULL)
    {
        lv_label_set_text_fmt(s_servo_num, "%3d", (int)s_angle);
    }
}

/* 主菜单当前选中项 —— App.c 需要知道用户选的是哪个, 好决定进哪一屏。
   s_sel 由 Ui_RowFocusCb 在焦点变化时维护, 所以这里直接读就行。
   ⚠ 之所以不走动作回调: "选中"是界面的内部状态, "进入"才是动作。
     把两者混进同一个回调会让 App.c 收到一堆无意义的中间通知。 */
uint8_t UiScreens_GetSel(void)
{
    return s_sel;
}
