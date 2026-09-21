/**
  ******************************************************************************
  * @file    SoftWare/src/Menu.c
  * @brief   三个界面的绘制实现 —— 详见 Menu.h 的说明(本模块只管画, 不碰硬件)
  *
  *          屏幕 128x160, (0,0) 是用户看到的左上角(MADCTL=0xC0)。
  *          汉字 16x16, ASCII 8x16。
  *
  *          绘制开销的几点注意(踩过再改会很费时间):
  *            - **不要用 LCD_DrawPoint()**: 它内部是 LCD_Fill(x,y,1,1), 每画
  *              一个点都要走"设窗口 -> 切 8/16 位 -> 配 DMA -> 等 DMA"整套
  *              流程, 几十微秒一个点。要画东西一律用 LCD_Fill / LCD_DrawRect /
  *              LCD_ShowChar / LCD_ShowCN。
  *            - **不要在两帧之间 LCD_Clear()**: 它是 20480 像素的 DMA, 约 15.6ms,
  *              每帧来一次会明显发滞。只在切换界面时清一次。
  ******************************************************************************
  */

#include "Menu.h"
#include "LCD.h"

/* ============================ 布局常量 ============================ */
/* 颜色 */
#define C_BG        LCD_BLACK
#define C_TITLE     LCD_WHITE
#define C_ITEM      LCD_WHITE
#define C_SEL       LCD_YELLOW      /* 选中: 箭头 + 方框 */
#define C_HINT      LCD_GRAY
#define C_VALUE     LCD_YELLOW
#define C_BAR       LCD_GREEN

/* 主菜单 */
#define M_TITLE_X       40U
#define M_TITLE_Y        6U
#define M_ROW0_Y        44U         /* 第 0 行方框的顶边 */
#define M_ROW_STEP      32U         /* 行间距(方框高 22 + 空隙 10) */
#define M_BOX_X         16U
#define M_BOX_W        112U         /* 16 + 112 = 128, 正好顶到右边缘 */
#define M_BOX_H         22U
#define M_ITEM_X        32U         /* 行文字 */
#define M_ARROW_X        4U         /* 箭头在方框左边外面 */
#define M_HINT1_Y      124U
#define M_HINT2_Y      142U

/* 舵机界面 */
#define S_TITLE_X       48U
#define S_TITLE_Y        6U
#define S_LABEL_X       24U         /* "角度" */
#define S_LABEL_Y       48U
#define S_NUM_X         60U         /* 3 位定宽数字 */
#define S_NUM_Y         48U
#define S_UNIT_X        88U         /* "度" */
#define S_UNIT_Y        48U
#define S_BAR_X          8U
#define S_BAR_Y         80U
#define S_BAR_W        112U
#define S_BAR_H         16U
#define S_BAR_PAD        2U          /* 填充与外框之间的留白 */
#define S_BAR_IN_W     (S_BAR_W - 2U * S_BAR_PAD)   /* 108 */
#define S_BAR_IN_H     (S_BAR_H - 2U * S_BAR_PAD)   /* 12 */
#define S_HINT1_Y      120U
#define S_HINT2_Y      140U

/* 摄像头界面 */
#define CAM_IMG_X        4U         /* 画面 120 宽, 居中: (128-120)/2 = 4 */
#define CAM_FPS_LBL_X   80U         /* "帧率" 32px  -> 80..111 */
#define CAM_FPS_NUM_X  112U         /* 2 位数字 16px -> 112..127 */
#define CAM_FPS_Y        0U

/* 菜单项名字, 顺序必须和 Menu.h 的 menu_item_t 一致 */
static const char *const s_items[MENU_ITEM_COUNT] =
{
    "摄像头",       /* MENU_ITEM_CAMERA */
    "舵机",         /* MENU_ITEM_SERVO  */
};

/* ============================ 内部工具 ============================ */
/* 画定宽的十进制数: 不足 digits 位时高位补**空格**(不是补 '0')。
   为什么非要定宽 —— LCD_ShowInt() 只画实际需要的位数, 角度从 120 变成 99 时
   它把 "99" 画在开头, 原来第 3 位那个 '0' 就留在屏幕上, 显示成 "990";
   FPS 从 10 掉到 9 同理。定宽之后位数恒定, 既不残影也不闪。

   v=90,  digits=3  ->  " 90"
   v=8,   digits=2  ->  " 8"
   v=0,   digits=3  ->  "  0" */
static void Menu_DrawNum(uint16_t x, uint16_t y, uint16_t v,
                         uint8_t digits, uint16_t fg, uint16_t bg)
{
    char    buf[6];
    uint16_t t = v;
    uint8_t  i;

    if ((digits == 0U) || (digits > 5U)) { digits = 5U; }

    /* 先按定宽拆成数字(带前导零) */
    for (i = digits; i > 0U; i--)
    {
        buf[i - 1U] = (char)('0' + (t % 10U));
        t /= 10U;
    }
    buf[digits] = '\0';

    /* 再把前导零换成空格(最高位永远保留, v=0 时显示 "  0" 而不是全空) */
    for (i = 0U; (uint8_t)(i + 1U) < digits; i++)
    {
        if (buf[i] != '0') { break; }
        buf[i] = ' ';
    }

    LCD_ShowString(x, y, buf, fg, bg);
}

/* 画一行菜单项的文字 */
static void Menu_DrawItemText(uint8_t row)
{
    if (row >= (uint8_t)MENU_ITEM_COUNT) { return; }
    LCD_ShowCN(M_ITEM_X,
               (uint16_t)(M_ROW0_Y + (uint16_t)row * M_ROW_STEP + 3U),
               s_items[row], C_ITEM, C_BG);
}

/* ============================ 主菜单 ============================ */
void Menu_DrawMain(uint8_t sel)
{
    uint8_t r;

    LCD_Clear(C_BG);

    LCD_ShowCN(M_TITLE_X, M_TITLE_Y, "主菜单", C_TITLE, C_BG);

    /* 每一项先只画文字; 选中效果(箭头+方框)由 Menu_DrawCursor 单独画,
       这样切换选中时不用整屏重画 */
    for (r = 0U; r < (uint8_t)MENU_ITEM_COUNT; r++)
    {
        Menu_DrawItemText(r);
    }

    LCD_ShowCN(32U, M_HINT1_Y, "旋转选择", C_HINT, C_BG);
    LCD_ShowCN(32U, M_HINT2_Y, "按下进入", C_HINT, C_BG);

    Menu_DrawCursor(sel, 1U);
}

void Menu_DrawCursor(uint8_t row, uint8_t on)
{
    uint16_t y;

    if (row >= (uint8_t)MENU_ITEM_COUNT) { return; }
    y = (uint16_t)(M_ROW0_Y + (uint16_t)row * M_ROW_STEP + 3U);

    if (on != 0U)
    {
        /* '>' 在字库里就是一条完整的斜箭头(0x3E), 不用自己拿点拼 */
        LCD_ShowChar(M_ARROW_X, y, '>', C_SEL, C_BG);
        LCD_DrawRect(M_BOX_X, (uint16_t)(y - 3U), M_BOX_W, M_BOX_H, C_SEL, 0U);
    }
    else
    {
        /* 擦掉: 空格的点阵全是 0, 会把整格刷成 bg */
        LCD_ShowChar(M_ARROW_X, y, ' ', C_BG, C_BG);
        /* 方框原地重画成背景色 = 擦掉 */
        LCD_DrawRect(M_BOX_X, (uint16_t)(y - 3U), M_BOX_W, M_BOX_H, C_BG, 0U);
    }
}

/* ============================ 舵机界面 ============================ */
void Menu_DrawServoChrome(void)
{
    LCD_Clear(C_BG);

    LCD_ShowCN(S_TITLE_X, S_TITLE_Y, "舵机", C_TITLE, C_BG);

    LCD_ShowCN(S_LABEL_X, S_LABEL_Y, "角度", C_ITEM,  C_BG);
    LCD_ShowCN(S_UNIT_X,  S_UNIT_Y,  "度",   C_ITEM,  C_BG);

    /* 进度条外框(不填充, 空框) */
    LCD_DrawRect(S_BAR_X, S_BAR_Y, S_BAR_W, S_BAR_H, C_HINT, 0U);

    LCD_ShowCN(32U, S_HINT1_Y, "旋转调角", C_HINT, C_BG);
    LCD_ShowCN(32U, S_HINT2_Y, "按下退出", C_HINT, C_BG);
    /* 角度数字和进度条填充由 Menu_DrawServoValue() 画 —— 进入界面时
       调用方会用当前角度调一次, 所以这里不用管 */
}

void Menu_DrawServoValue(uint8_t deg)
{
    uint16_t fill_w;

    if (deg > 180U) { deg = 180U; }

    Menu_DrawNum(S_NUM_X, S_NUM_Y, deg, 3U, C_VALUE, C_BG);

    /* ⚠ 必须先把整条填充区清干净再画新的。只画"新长度"的话, 角度变小时
       上一次留下的那截绿条还在, 看着像进度条"缩不回去"。 */
    LCD_Fill(S_BAR_X + S_BAR_PAD, S_BAR_Y + S_BAR_PAD,
             S_BAR_IN_W, S_BAR_IN_H, C_BG);

    fill_w = (uint16_t)(((uint32_t)S_BAR_IN_W * deg) / 180U);
    if (fill_w > 0U)
    {
        LCD_Fill(S_BAR_X + S_BAR_PAD, S_BAR_Y + S_BAR_PAD,
                 fill_w, S_BAR_IN_H, C_BAR);
    }
}

/* ============================ 摄像头界面 ============================ */
void Menu_DrawCameraChrome(void)
{
    LCD_Clear(C_BG);
    /* 这里**不画**"帧率"标签, 交给 Menu_DrawCameraFps() —— 理由见下 */
    Menu_DrawCameraFps(0U);             /* 第一帧到达前先占个位, 免得空着 */
}

/* ⚠ 必须**每一帧**都调, 而且要在 LCD_DrawImage() 之后调。
   注意"帧率"这两个字也要一起重画: 画面从 x=4 铺到 x=123, 而标签在
   x=80~111 —— 整块都会被 LCD_DrawImage() 覆盖掉, 只重画数字的话
   标签第一帧之后就消失了。这也是 Chrome 里不画标签的原因。
   开销: 2 个汉字 + 2 位数字 ≈ 0.6ms, 相对一轮 95ms 可以忽略。 */
void Menu_DrawCameraFps(uint8_t fps)
{
    if (fps > 99U) { fps = 99U; }       /* 定宽 2 位, 超了也画不下 */

    LCD_ShowCN(CAM_FPS_LBL_X, CAM_FPS_Y, "帧率", C_ITEM, C_BG);
    Menu_DrawNum(CAM_FPS_NUM_X, CAM_FPS_Y, fps, 2U, C_VALUE, C_BG);
}

void Menu_DrawCameraNoSignal(void)
{
    /* 居中: "无信号" 3 字 x 16 = 48px -> (128-48)/2 = 40 */
    LCD_ShowCN(40U, 72U, "无信号", C_VALUE, C_BG);
}
