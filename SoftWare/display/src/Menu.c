/**
  ******************************************************************************
  * @file    SoftWare/src/Menu.c
  * @brief   三个界面的绘制实现 —— 详见 Menu.h 的说明(本模块只管画, 不碰硬件)
  *
  *          屏幕 128x160, (0,0) 是用户看到的左上角(MADCTL=0xC0)。
  *          汉字 16x16, ASCII 8x16。
  *
  *          ---- 视觉规范(三个界面统一, 2026-09-22 重做) ----
  *            · 顶部反色标题栏(高 18)  + 一条分隔线, 每屏都有
  *            · 主菜单每项占两行: 大字是名字, 下面一行灰色小字说明用途
  *            · 每一项都带方框, 未选中灰框、选中黄框 + 左侧 '>' 箭头
  *            · 强调色统一用黄, 说明文字统一用灰
  *
  *          绘制开销的几点注意(踩过再改会很费时间):
  *            - **不要用 LCD_DrawPoint()**: 它内部是 LCD_Fill(x,y,1,1), 每画
  *              一个点都要走完整流程(设窗口 -> 切 8/16 位 -> 配 DMA -> 等 DMA),
  *              几十微秒一个点。要画东西一律用 LCD_Fill / LCD_DrawRect /
  *              LCD_ShowChar / LCD_ShowCN。
  *            - **不要在两帧之间 LCD_Clear()**: 它约 15.6ms, 每帧来一次会明显
  *              发滞。只在切换界面时清一次。
  *            - **同一个东西重画时只画变化的部分**, 别"先清后画" —— 中间那个
  *              空状态会被眼睛看到(进度条就是这么闪的, 见 Menu_DrawServoValue)。
  ******************************************************************************
  */

#include "Menu.h"
#include "LCD.h"

#include "FreeRTOS.h"
#include "semphr.h"

/* ============================ LCD 互斥量 ============================
   UiTask 和 CameraTask 都会画屏: UiTask 画菜单/舵机界面, CameraTask 画摄像头
   画面。不加锁的话, CameraTask 那 14.6ms 的整块 DMA 传输可能插在 UiTask 一次
   界面重画的中间 —— 而 LCD 的"设窗口"和"发数据"是两步, 被插进去就会**花屏**
   (菜单里混进几行摄像头像素)。

   锁只放在这一处: 所有画屏都走本文件的公开函数, 由它们自己加锁。
   App.c 因此不用管"哪里该加锁"的纪律, 少一类出错的机会。

   ⚠ **公开函数内部不要再调另一个公开函数** —— 普通互斥量不可重入, 会死锁。
     要复用就抽成下面那些不带锁的 static 内部函数(例如 Menu_CursorDraw)。 */
static SemaphoreHandle_t s_lcd_lock = NULL;

/* ⚠ 这两个是**公开**的, 但正常情况下不该用 —— 所有 Menu_DrawXxx 自带加锁。
   只有一种场合例外: 当"要不要画"这个判断必须和"画"原子完成时
   (见 App.c 的 CameraTask: 它要在锁内复查界面有没有被切走)。
   其余地方请直接用那些自带锁的函数, 别手动加锁。 */
void Menu_Lock(void)
{
    /* 建锁失败(堆不够)会在别处先报出来, 这里兜一下别解引用空指针 */
    if (s_lcd_lock != NULL) { (void)xSemaphoreTake(s_lcd_lock, portMAX_DELAY); }
}

void Menu_Unlock(void)
{
    if (s_lcd_lock != NULL) { (void)xSemaphoreGive(s_lcd_lock); }
}

void Menu_Init(void)
{
    s_lcd_lock = xSemaphoreCreateMutex();
}

/* ============================ 颜色 ============================ */
#define C_BG        LCD_BLACK
#define C_BAR_BG    LCD_BLUE        /* 标题栏底色(反色, 每屏都有一条) */
#define C_BAR_FG    LCD_WHITE       /* 标题文字 */
#define C_SEP       0x4208U         /* 分隔线: 深灰, 只是轻轻划一道 */
#define C_ITEM      LCD_WHITE       /* 菜单项名字 */
#define C_SUB       LCD_GRAY        /* 菜单项下面那行说明 */
#define C_SEL       LCD_YELLOW      /* 选中: 箭头 + 方框 + 名字 */
#define C_BOX_IDLE  0x4208U         /* 未选中项的方框: 深灰, 存在感低但不消失 */
#define C_HINT      LCD_GRAY
#define C_VALUE     LCD_YELLOW
#define C_BAR       LCD_GREEN

/* ============================ 布局常量 ============================ */
/* 三屏共用 */
#define L_TITLEBAR_H    18U         /* 顶部标题栏高度 */
#define L_TITLEBAR_TY    1U         /* 标题文字在栏内的 y(栏 18, 字 16, 上下各 1) */
#define L_SEP_Y         20U         /* 标题栏下的分隔线 */

/* ---- 主菜单 ---- */
#define M_ROW0_Y        26U         /* 第 0 项方框的顶边 */
#define M_ROW_STEP      46U         /* 行间距(方框高 42 + 空隙 4) */
#define M_BOX_X         16U
#define M_BOX_W        112U         /* 16 + 112 = 128, 正好顶到右边缘 */
#define M_BOX_H         42U         /* 两行字: 名字 + 说明 */
#define M_TEXT_X        32U         /* 名字和说明用同一个 x, 左对齐才整齐 */
#define M_NAME_DY        3U         /* 名字相对方框顶边的偏移 */
#define M_SUB_DY        23U         /* 说明相对方框顶边的偏移 */
#define M_ARROW_X        0U         /* 箭头在方框左边外面 */
#define M_FOOT_SEP_Y   118U
#define M_HINT1_Y      122U
#define M_HINT2_Y      140U

/* ---- 舵机界面 ---- */
#define S_LABEL_X       24U         /* "角度" */
#define S_NUM_X         60U         /* 3 位定宽数字 */
#define S_UNIT_X        88U         /* "度" */
#define S_ROW_Y         40U
#define S_BAR_X          8U
#define S_BAR_Y         76U
#define S_BAR_W        112U
#define S_BAR_H         18U
#define S_BAR_PAD        2U          /* 填充与外框之间的留白 */
#define S_BAR_IN_W     (S_BAR_W - 2U * S_BAR_PAD)   /* 108 */
#define S_BAR_IN_H     (S_BAR_H - 2U * S_BAR_PAD)   /* 14 */
#define S_SCALE_Y       98U         /* 进度条下面的 0 / 180 刻度 */
#define S_SCALE_L_X      8U         /* 与进度条左边缘对齐 */
#define S_SCALE_R_X     96U         /* "180" 宽 24px -> 96..119, 与右边缘对齐 */
#define S_HINT1_Y      122U
#define S_HINT2_Y      140U

/* ---- 摄像头界面 ---- */
/* 画面 120x160 铺满整屏, **不画标题栏** —— 它本来就是全屏预览,
   压一条栏进去会白白损失 18 行的画面。帧率用小块叠加在右上角。 */
#define CAM_IMG_X        4U
#define CAM_FPS_LBL_X   80U         /* "帧率" 32px  -> 80..111 */
#define CAM_FPS_NUM_X  112U         /* 2 位数字 16px -> 112..127 */
#define CAM_FPS_Y        0U

/* 菜单项的名字和说明, 顺序必须和 Menu.h 的 menu_item_t 一致。
   说明控制在 5 个汉字以内 —— 文字从 x=32 起, 方框右边缘在 127,
   留 4px 内边距的话可用宽度约 91px, 5 个汉字(80px)刚好放得下。 */
static const char *const s_item_name[MENU_ITEM_COUNT] =
{
    "摄像头",       /* MENU_ITEM_CAMERA */
    "舵机",         /* MENU_ITEM_SERVO  */
};
static const char *const s_item_sub[MENU_ITEM_COUNT] =
{
    "实时画面",     /* MENU_ITEM_CAMERA */
    "旋转调角度",   /* MENU_ITEM_SERVO  */
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

/* 顶部标题栏 + 分隔线。三屏共用, 保证视觉一致。 */
static void Menu_DrawTitleBar(const char *title)
{
    LCD_Fill(0U, 0U, LCD_W, L_TITLEBAR_H, C_BAR_BG);

    /* 标题居中: 先算文字宽度再定起点 */
    LCD_ShowCN((uint16_t)((LCD_W - LCD_CNWidth(title)) / 2U),
               L_TITLEBAR_TY, title, C_BAR_FG, C_BAR_BG);

    /* 分隔线, 1 像素高 */
    LCD_Fill(0U, L_SEP_Y, LCD_W, 1U, C_SEP);
}

/* 画一项的名字 + 说明(不含方框和箭头, 那两个由 Menu_DrawCursor 负责) */
static void Menu_DrawItemText(uint8_t row, uint16_t fg)
{
    uint16_t top;

    if (row >= (uint8_t)MENU_ITEM_COUNT) { return; }

    top = (uint16_t)(M_ROW0_Y + (uint16_t)row * M_ROW_STEP);

    LCD_ShowCN(M_TEXT_X, (uint16_t)(top + M_NAME_DY), s_item_name[row], fg,      C_BG);
    LCD_ShowCN(M_TEXT_X, (uint16_t)(top + M_SUB_DY),  s_item_sub[row],  C_SUB,   C_BG);
}

/* 不带锁的内部版, 定义在下面 —— Menu_DrawMain 要先声明才能用 */
static void Menu_CursorDraw(uint8_t row, uint8_t on);

/* ============================ 主菜单 ============================ */
void Menu_DrawMain(uint8_t sel)
{
    uint8_t r;

    Menu_Lock();                    /* 整屏重画期间不许别人插进来 */

    LCD_Clear(C_BG);

    Menu_DrawTitleBar("主菜单");

    /* 先把所有项按"未选中"画一遍(名字白、方框深灰),
       选中效果由 Menu_CursorDraw 单独叠上去 —— 这样切换选中时
       不用整屏重画 */
    for (r = 0U; r < (uint8_t)MENU_ITEM_COUNT; r++)
    {
        LCD_DrawRect(M_BOX_X,
                     (uint16_t)(M_ROW0_Y + (uint16_t)r * M_ROW_STEP),
                     M_BOX_W, M_BOX_H, C_BOX_IDLE, 0U);
        Menu_DrawItemText(r, C_ITEM);
    }

    /* 底部: 一条分隔线 + 两行操作提示 */
    LCD_Fill(0U, M_FOOT_SEP_Y, LCD_W, 1U, C_SEP);
    LCD_ShowCN(32U, M_HINT1_Y, "旋转选择", C_HINT, C_BG);
    LCD_ShowCN(32U, M_HINT2_Y, "按下进入", C_HINT, C_BG);

    /* ⚠ 这里必须调**不带锁**的内部版 —— 锁已经拿在手里了,
       再调带锁的 Menu_DrawCursor 会嵌套加锁 -> 死锁 */
    Menu_CursorDraw(sel, 1U);

    Menu_Unlock();
}

/* 切换选中时只动这一项: 方框颜色、箭头、名字颜色。
   on=0 是"恢复成未选中" —— 注意方框不是擦掉而是**重画成深灰**,
   因为现在每一项都带框, 擦出个白洞反而更怪。

   ⚠ 这是**不带锁**的内部版, 只给已经持有锁的 Menu_DrawMain 用。
     外部调用请用下面的 Menu_DrawCursor()。 */
static void Menu_CursorDraw(uint8_t row, uint8_t on)
{
    uint16_t top;

    if (row >= (uint8_t)MENU_ITEM_COUNT) { return; }
    top = (uint16_t)(M_ROW0_Y + (uint16_t)row * M_ROW_STEP);

    if (on != 0U)
    {
        LCD_DrawRect(M_BOX_X, top, M_BOX_W, M_BOX_H, C_SEL, 0U);
        LCD_ShowChar(M_ARROW_X, (uint16_t)(top + M_NAME_DY), '>', C_SEL, C_BG);
        Menu_DrawItemText(row, C_SEL);
    }
    else
    {
        LCD_DrawRect(M_BOX_X, top, M_BOX_W, M_BOX_H, C_BOX_IDLE, 0U);
        /* 擦箭头: 空格点阵全 0, 会把整格刷成 bg */
        LCD_ShowChar(M_ARROW_X, (uint16_t)(top + M_NAME_DY), ' ', C_BG, C_BG);
        Menu_DrawItemText(row, C_ITEM);
    }
}

/* 带锁的公开版: 转动编码器切换选中时调它 */
void Menu_DrawCursor(uint8_t row, uint8_t on)
{
    Menu_Lock();
    Menu_CursorDraw(row, on);
    Menu_Unlock();
}

/* ============================ 舵机界面 ============================ */
/* 进度条当前画到多宽。Menu_DrawServoValue() 靠它算出"哪一段需要重画",
   从而避免整条清空再重画导致的闪动(详见那个函数的注释)。
   进入界面时必须在 Chrome 里归零, 否则上一屏留下的宽度会让第一次
   增量更新算错范围。 */
static uint16_t s_bar_last_w = 0U;

void Menu_DrawServoChrome(void)
{
    Menu_Lock();

    LCD_Clear(C_BG);

    s_bar_last_w = 0U;          /* 屏已清空, 同步复位进度条的"已画宽度" */

    Menu_DrawTitleBar("舵机");

    LCD_ShowCN(S_LABEL_X, S_ROW_Y, "角度", C_ITEM, C_BG);
    LCD_ShowCN(S_UNIT_X,  S_ROW_Y, "度",   C_SUB,  C_BG);

    /* 进度条外框: 里面什么都不画, 填充交给 Menu_DrawServoValue */
    LCD_DrawRect(S_BAR_X, S_BAR_Y, S_BAR_W, S_BAR_H, C_SUB, 0U);

    /* 两端刻度, 让那条绿条有个参照 —— 顺便填掉进度条到脚分隔线之间的空白 */
    LCD_ShowString(S_SCALE_L_X, S_SCALE_Y, "0",   C_SUB, C_BG);
    LCD_ShowString(S_SCALE_R_X, S_SCALE_Y, "180", C_SUB, C_BG);

    LCD_Fill(0U, M_FOOT_SEP_Y, LCD_W, 1U, C_SEP);
    LCD_ShowCN(32U, S_HINT1_Y, "旋转调角", C_HINT, C_BG);
    LCD_ShowCN(32U, S_HINT2_Y, "按下退出", C_HINT, C_BG);

    Menu_Unlock();
}

void Menu_DrawServoValue(uint8_t deg)
{
    uint16_t fill_w;

    if (deg > 180U) { deg = 180U; }

    Menu_Lock();

    Menu_DrawNum(S_NUM_X, S_ROW_Y, deg, 3U, C_VALUE, C_BG);

    fill_w = (uint16_t)(((uint32_t)S_BAR_IN_W * deg) / 180U);

    /* ⚠ 进度条不要"先整条清成背景色、再画新条"。
       那两次 LCD_Fill 之间有一瞬间进度条是**空的**(各自都要走一遍
       "设窗口 -> 配 DMA -> 等传完"), 转动时每格都闪一下 —— 就是之前
       "偶尔闪动"的来源; 串口诊断开着时主循环被拖慢, 空窗拉长, 更明显。

       改成**只重画变化的那一段**:
         变长 -> 只在新增的那截上补绿色
         变短 -> 只把多出来的那截擦成背景色
       屏幕上任何时刻都是连续的进度条, 没有空窗; 顺带少画一半像素。 */
    if (fill_w > s_bar_last_w)
    {
        LCD_Fill((uint16_t)(S_BAR_X + S_BAR_PAD + s_bar_last_w), S_BAR_Y + S_BAR_PAD,
                 (uint16_t)(fill_w - s_bar_last_w), S_BAR_IN_H, C_BAR);
    }
    else if (fill_w < s_bar_last_w)
    {
        LCD_Fill((uint16_t)(S_BAR_X + S_BAR_PAD + fill_w), S_BAR_Y + S_BAR_PAD,
                 (uint16_t)(s_bar_last_w - fill_w), S_BAR_IN_H, C_BG);
    }

    s_bar_last_w = fill_w;

    Menu_Unlock();
}

/* ============================ 摄像头界面 ============================ */
/* 不带锁的内部版: 画右上角那个"帧率 xx"。
   ⚠ 每一帧都要重画, 而且必须在画面之后 —— 画面从 x=4 铺到 x=123,
     而标签在 x=80~111, 整块都会被 LCD_DrawImage() 覆盖掉, 只重画数字的话
     标签第一帧之后就消失了。这也是 Chrome 里不画标签的原因。 */
static void Menu_CameraFpsDraw(uint8_t fps)
{
    if (fps > 99U) { fps = 99U; }       /* 定宽 2 位, 超了也画不下 */

    LCD_ShowCN(CAM_FPS_LBL_X, CAM_FPS_Y, "帧率", C_BAR_FG, C_BG);
    Menu_DrawNum(CAM_FPS_NUM_X, CAM_FPS_Y, fps, 2U, C_VALUE, C_BG);
}

void Menu_DrawCameraChrome(void)
{
    Menu_Lock();

    LCD_Clear(C_BG);
    Menu_CameraFpsDraw(0U);             /* 第一帧到达前先占个位, 免得空着 */

    Menu_Unlock();
}

/* 摄像头画面 + 帧率。
 *
 * ⚠ **本函数不加锁**, 调用方负责。用法(见 App.c 的 CameraTask):
 *
 *     Menu_Lock();
 *     if (s_screen == UI_CAMERA) { Menu_DrawCameraFrameLocked(buf, fps); }
 *     Menu_Unlock();
 *
 * 为什么非要"锁内判断 + 锁内画"这个组合 —— 这是整个任务拆分里唯一一处
 * 需要小心的并发:
 *
 *   UiTask 切走界面时是「先把 s_screen 改成 MENU, 再去画菜单(画的时候加锁)」。
 *   CameraTask 如果**在锁外**判断"还在摄像头界面吗", 存在这样一个窗口:
 *     它判断完(还在) -> UiTask 改状态并抢到锁画完菜单 -> CameraTask 才拿到锁
 *     把那帧相机画面**盖到刚画好的菜单上**, 而且没人会再重画菜单, 就这么花着。
 *
 *   把判断挪进锁里之后, 两种交错都收敛到同一个结果:
 *     · CameraTask 先拿到锁 -> UiTask 改完状态后在锁上等 -> 相机帧先画,
 *       菜单随后盖上去 -> **菜单赢** ✓
 *     · UiTask 先拿到锁 -> CameraTask 等到锁时 s_screen 已经是 MENU -> 直接跳过 ✓
 */
void Menu_DrawCameraFrameLocked(const uint16_t *buf, uint8_t fps)
{
    LCD_DrawImage(CAM_IMG_X, 0U, 120U, 160U, buf);
    Menu_CameraFpsDraw(fps);
}

void Menu_DrawCameraNoSignal(void)
{
    Menu_Lock();

    /* 居中: "无信号" 3 字 x 16 = 48px -> (128-48)/2 = 40 */
    LCD_ShowCN(40U, 72U, "无信号", C_VALUE, C_BG);

    Menu_Unlock();
}
