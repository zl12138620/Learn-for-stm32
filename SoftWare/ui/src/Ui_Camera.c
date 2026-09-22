/**
  ******************************************************************************
  * @file    SoftWare/ui/src/Ui_Camera.c
  * @brief   摄像头界面实现 —— 见 Ui_Camera.h 的说明(线程模型和栅栏方向)
  ******************************************************************************
  */

#include "Ui_Camera.h"
#include "Ui_Screens.h"
#include "ui_theme.h"

#include "lvgl.h"

/* ============================ 布局 ============================ */
/* 画面占满整屏, 只有右上角一个小片显示帧率。
   ⚠ 帧率**做成一个小片(chip)而不是直接把字压在画面上**:
     底下的实时画面内容不可控(亮场景/暗场景都有), 白字直接压上去
     在某些画面上会看不清。加一层比背景亮一档的底就稳了。
     这也是 lvgl-ui skill 里 UI_SURFACE 这个令牌的用途之一。 */
#define FPS_CHIP_W   30U
#define FPS_CHIP_H   22U
#define FPS_CHIP_X   (128U - UI_MARGIN - FPS_CHIP_W)   /* 贴右上角 */
#define FPS_CHIP_Y    4U
#define NOSIG_Y      72U        /* "无信号" 居中: 三个汉字 48px, 128-48 -> x=40 */

/* ============================ 状态 ============================ */

/* 图片描述符。
   ⚠ **不是拷贝** —— data 直接指向 OV7670 的帧缓冲, 每帧原地更新。
     所以它必须是**可写的**(不能加 const 丢进 .rodata), data 在 Build 时填。
   ⚠ 为什么可以这样: 对未压缩的 LV_IMAGE_SRC_VARIABLE 图片,
     lv_bin_decoder 只是把 image->data 包一层就返回
     (src/image/lv_bin_decoder.c 的 use_directly 分支, 在写图片缓存**之前**
      就 return 了), 所以每次绘制都重新读这个指针。
     推论: **不需要**每帧重新 lv_image_set_src(), 那只会白跑一遍解码器信息查询。 */
static lv_image_dsc_t s_cam_dsc;

static lv_obj_t *s_root      = NULL;    /* 本界面的屏幕对象 */
static lv_obj_t *s_img       = NULL;    /* 画面控件(没探到摄像头时为 NULL) */
static lv_obj_t *s_fps_num   = NULL;    /* 帧率数字 */
static lv_obj_t *s_no_signal = NULL;    /* "无信号" */
static lv_obj_t *s_exit      = NULL;    /* 全屏透明按钮, 只为"按下退出" */

/* 帧序号。CameraTask 写, UiTask 读。单个 32 位字的读写是原子的, 所以不加锁。 */
static volatile uint32_t s_cam_seq      = 0U;
static uint32_t          s_cam_seq_seen = 0U;

/* 渲染栅栏。UiTask 写, CameraTask 读。见 Ui_Camera.h 的线程模型。 */
static volatile uint8_t  s_rendering    = 0U;

static uint8_t s_fps_shown = 0xFFU;     /* 0xFF = 还没画过, 保证第一帧一定写 */

/* ========================================================================
 * 事件回调
 * ====================================================================== */

/* "按下退出" —— 转交给 App.c 的状态机。
   ⚠ 注意屏幕是**全屏**按钮: 编码器只要在摄像头界面按一下, 就会命中它。
     这正是我们要的(唯一的可聚焦控件)。 */
static void UiCam_ExitCb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        UiScreens_Notify(UI_ACT_BACK_MAIN, 0U);
    }
}

/* ========================================================================
 * 建界面
 * ====================================================================== */
void UiCam_Build(const uint16_t *buf)
{
    lv_obj_t *chip;

    /* ---- 屏幕本体 ----
       lv_obj_create(NULL) 建的是一个**屏幕对象**(parent 传 NULL 就是这个意思),
       之后由 UiScreens_Enter 用 lv_screen_load 挂上去。 */
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);

    /* ⚠ 关掉滚动。默认是开的, 而 lv_button 的构造里还会设 scroll_on_focus ——
       不关的话, 焦点落到那个全屏按钮上时整屏会被"滚动", 画面看着会跑偏。 */
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_style_bg_color(s_root, UI_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_root, 0, LV_PART_MAIN);

    /* ---- 画面 ----
       ⚠ **对象尺寸必须等于图片尺寸(120x160), 不能写成屏幕的 128x160。**
          lv_image 的绘制区是按 obj->coords + 图片自身的 w/h 算的; 两者不一致
          时它会把画面**居中**, 于是 (4,0) 就不再是画面左上角了。
          现象很迷惑: 画面能出来, 只是左右各偏 4 像素 —— "看着差不多但不对"。 */
    if (buf != NULL)
    {
        s_cam_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_cam_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_cam_dsc.header.flags  = 0U;               /* 绝不能有 ALLOCATED/COMPRESSED */
        s_cam_dsc.header.w      = UICAM_W;
        s_cam_dsc.header.h      = UICAM_H;
        s_cam_dsc.header.stride = UICAM_W * 2U;     /* 显式写, 见头文件说明 */
        s_cam_dsc.data_size     = UICAM_W * UICAM_H * 2U;
        s_cam_dsc.data          = (const uint8_t *)buf;

        s_img = lv_image_create(s_root);
        lv_obj_remove_style_all(s_img);             /* 主题对 lv_image 本来也没样式, 显式更放心 */
        lv_obj_set_pos(s_img, UICAM_X, UICAM_Y);
        lv_obj_set_size(s_img, UICAM_W, UICAM_H);
        lv_image_set_src(s_img, &s_cam_dsc);        /* 只调这一次 */
    }

    /* ---- 帧率小片 ----
       ⚠ 建在画面**之后**, 靠"创建顺序 = 绘制顺序"自然叠在画面上面。
         老代码做不到这一点, 只能每帧把标签重画一遍(画面会把它盖掉),
         见 Menu.c 里 Menu_CameraFpsDraw 的注释。 */
    chip = lv_obj_create(s_root);
    lv_obj_remove_style_all(chip);
    lv_obj_set_pos(chip, FPS_CHIP_X, FPS_CHIP_Y);
    lv_obj_set_size(chip, FPS_CHIP_W, FPS_CHIP_H);
    lv_obj_set_style_radius(chip, UI_RADIUS_SM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, UI_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, UI_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);

    /* 数字居中放在小片里。
       ⚠ 只放数字、不放"帧率"两个字 —— 在实时画面的右上角出现一个跳动的
         数字, 含义是自明的, 而"帧率"两个汉字(32px)加上数字放不进
         128 宽里的小片。skill 那条 "Colour = meaning / 去掉不编码信息的
         元素" 在这里正好适用。 */
    s_fps_num = lv_label_create(chip);
    lv_obj_remove_style_all(s_fps_num);
    lv_label_set_text(s_fps_num, " 0");             /* 定宽两位, 见 UiCam_SetFps */
    lv_obj_center(s_fps_num);
    lv_obj_set_style_text_color(s_fps_num, UI_WARN, LV_PART_MAIN);

    /* ---- "无信号" ----
       没探到摄像头时显示。用**隐藏标志**而不是建/删控件 ——
       建删会让 LVGL 的池子反复分配释放, 藏起来就一个标志位的事。 */
    s_no_signal = lv_label_create(s_root);
    lv_obj_remove_style_all(s_no_signal);
    lv_label_set_text(s_no_signal, "无信号");
    lv_obj_align(s_no_signal, LV_ALIGN_TOP_MID, 0, NOSIG_Y);
    lv_obj_set_style_text_color(s_no_signal, UI_WARN, LV_PART_MAIN);
    if (buf != NULL) { lv_obj_set_hidden(s_no_signal, true); }

    /* ---- 全屏透明按钮 —— 唯一的作用是给"按下退出"提供一个焦点对象 ----
       ⚠ 这个必须建, 不能省。分组为空时 lv_indev 的编码器分支会直接 return
         (见 LVGL/src/indev/lv_indev.c), 转动**和按下**全被丢掉,
         "按下退出"就永远不成立。
       ⚠ 建成全屏 + 透明: 转动在只有一个对象的组里是空操作(绕回自己),
         摄像头界面本来也不需要转动, 正好。
       ⚠ 建在最后 = 在最上层。它全透明, 所以只影响命中测试不影响观感。 */
    s_exit = lv_button_create(s_root);
    lv_obj_remove_style_all(s_exit);                /* 去掉主题给的蓝底 + 圆角 + 投影 */
    lv_obj_set_pos(s_exit, 0, 0);
    lv_obj_set_size(s_exit, 128, 160);
    lv_obj_set_style_bg_opa(s_exit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_exit, 0, LV_STATE_FOCUS_KEY);  /* 焦点圈也去掉 */
    lv_obj_set_scroll_on_focus(s_exit, false);
    lv_obj_add_event_cb(s_exit, UiCam_ExitCb, LV_EVENT_CLICKED, NULL);

    /* 复位帧序号, 免得切进来的第一帧用的是切换之前的旧画面 */
    s_cam_seq_seen = s_cam_seq;
    s_fps_shown    = 0xFFU;
}

void UiCam_Destroy(void)
{
    /* ⚠ lv_obj_delete 会把对象从它所在的分组里摘掉(lv_obj 的析构里做了),
       所以这里不用操心分组残留。 */
    if (s_root != NULL)
    {
        lv_obj_delete(s_root);
    }

    s_root      = NULL;
    s_img       = NULL;
    s_fps_num   = NULL;
    s_no_signal = NULL;
    s_exit      = NULL;
}

void *UiCam_Root(void)
{
    return (void *)s_root;
}

/* ========================================================================
 * 分组
 * ====================================================================== */
void UiCam_GroupAttach(void)
{
    lv_group_t *g = lv_group_get_default();

    if ((g == NULL) || (s_exit == NULL)) { return; }

    lv_group_add_obj(g, s_exit);
    lv_group_focus_obj(s_exit);
}

/* ========================================================================
 * 帧更新
 * ====================================================================== */

/* ⚠ 可以从 CameraTask 调 —— 只写一个 volatile 变量, 不碰任何 LVGL 状态。 */
void UiCam_MarkDirty(void)
{
    s_cam_seq++;
}

uint8_t UiCam_IsRendering(void)
{
    return s_rendering;
}

void UiCam_Pump(void)
{
    if ((s_root == NULL) || (s_img == NULL)) { return; }
    if (s_cam_seq == s_cam_seq_seen) { return; }        /* 没有新帧 */

    s_cam_seq_seen = s_cam_seq;

    /* 关栅栏 —— 从现在起到 lv_refr_now 返回, 我们正在从帧缓冲里读 */
    s_rendering = 1U;

    lv_obj_invalidate(s_img);

    /* ⚠ 这里必须用 lv_refr_now() 而**不是** lv_timer_handler()。
       原因: invalidate 只是把区域标脏; 真正渲染的是**刷新定时器**,
       它的周期是 LV_DEF_REFR_PERIOD(默认 33ms)。紧接着调 lv_timer_handler()
       时定时器多半还没到点, 于是什么都没渲, 栅栏白关一场空。
       lv_refr_now() 直接调 lv_display_refr_timer, 是**同步**的:
       返回时画面已经渲染完并 flush 完(本工程的 flush 回调是阻塞的),
       所以栅栏关着的时间正好等于"渲染 + 送屏"的真实耗时。 */
    lv_refr_now(NULL);

    s_rendering = 0U;                                   /* 开栅栏 */
}

/* ========================================================================
 * 帧率
 * ====================================================================== */
void UiCam_SetFps(uint8_t fps)
{
    if (s_fps_num == NULL) { return; }
    if (fps == s_fps_shown) { return; }                 /* 值没变就别动 LVGL */

    if (fps > 99U) { fps = 99U; }
    s_fps_shown = fps;

    /* ⚠ %2d 而不是 %d: 定宽两位。不定宽的话从 "10" 变成 "9" 会少一位,
       右边留下上一次的半个数字("9 0" 这种鬼东西)。老代码的 Menu_DrawNum
       就是为了这个才存在的。 */
    lv_label_set_text_fmt(s_fps_num, "%2d", (int)fps);

    /* 数字变了要让它重画。⚠ 只 invalidate 数字那一小块, 不是整个画面 ——
       LVGL 会自己算出脏矩形, 这正是它比手写驱动省事的地方。 */
    lv_obj_invalidate(s_fps_num);
}
