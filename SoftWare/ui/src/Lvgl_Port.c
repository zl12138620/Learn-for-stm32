/**
  ******************************************************************************
  * @file    SoftWare/ui/src/Lvgl_Port.c
  * @brief   LVGL 移植层实现 —— 时基 / 显示 / 输入 三个对接点
  *
  *          LVGL 要跟外界打交道的就三件事, 这里一件一件对上:
  *
  *            时基  lv_tick_set_cb(Tick_GetMs)
  *                  LVGL 内部所有定时(动画、长按、滚动惯性、flush 超时)
  *                  都基于一个"毫秒计数", 它不知道这个数从哪来 —— 给它
  *                  现成的 Tick_GetMs 就行。
  *
  *            显示  Lvgl_FlushCb
  *                  LVGL 把"屏幕上这块矩形脏了"的像素交给我们送。本工程
  *                  **白捡一个驱动**: 现成的 LCD_DrawImage(x,y,w,h,buf)
  *                  签名正好对上, 而且它内部就是 DMA 送 SPI2。
  *
  *            输入  Lvgl_EncReadCb(见文件末尾)
  *                  旋转编码器接到 LVGL 的 encoder 设备类型上。
  *                  LVGL 自带"焦点"概念: 转动移焦点, 按下触发 —— 正好
  *                  就是本工程原来手工实现的那套逻辑, 交给 LVGL 管。
  *
  *          ⚠ 线程模型见 Lvgl_Port.h: LV_USE_OS = LV_OS_NONE, 没有内部锁,
  *            所有东西都只在 UiTask 里跑。
  ******************************************************************************
  */

#include "Lvgl_Port.h"

#include "lvgl.h"       /* 第三方内核, 头文件在 LVGL/ 根目录(已加入 include 路径) */
#include "LCD.h"
#include "Tick.h"
#include "Encoder.h"
#include "Usart.h"      /* 诊断输出(见下面那节) */
#include "Led.h"        /* 断言失败时点灯报警 */

/* ==========================================================================
 * 诊断: 串口输出
 *
 *   和 App.c 里的 UI_DIAG / Diag_PrintLevels 是同一个套路 ——
 *   **让看不见的东西变成看得见的**, 而不是靠猜。
 *
 *   起因: 第一次烧录后屏幕上什么都没有, 而可能的原因有一长串
 *   (内核没起来 / 显示设备没建成 / 事件没触发 flush / 送了但屏没显示),
 *   光看现象完全分不开。把每一步打出来, 一眼就能定位到是哪一步断的。
 *
 *   ⚠ 这里直接用 comms 领域的 Usart —— 也就是 ui 依赖 comms。
 *     这是**向下**依赖, 方向没问题(记得到 SoftWare/README.md 的依赖表里补上)。
 *     为什么不交给 App.c 代打(App.c 里 Diag_PrintLevels 就是这么做的):
 *     LVGL 的**日志回调**和**断言处理函数**都是 LVGL 反过来调我们的,
 *     只能写在移植层里, 搬不到胶水层去。
 * ========================================================================== */

/* flush 次数。⚠ 定义在 #if 外面: 计数本身不受诊断开关影响,
   只有"打印出来"这件事才受 LVGL_DIAG 控制。 */
static uint32_t s_flush_cnt = 0U;

#if (LVGL_DIAG != 0)

/* 日志条数上限。串口 9600 一个字节约 1ms, 不限量的话光是打印就能把系统
   拖成"看起来卡死" —— 那反而会掩盖真正要查的问题。 */
#define LVGL_LOG_MAX   20U

static uint32_t s_log_cnt = 0U;

static void Lvgl_TraceStr(const char *s)
{
    uint32_t n = 0U;

    if (s == NULL) { return; }

    while (s[n] != '\0') { n++; }       /* 本工程没有现成的 strlen 封装 */
    Usart_SendBytes((const uint8_t *)s, n);
}

/* 打十进制。⚠ 不调 printf: 本工程没实现 newlib 的 _write, printf 出去
   不是没输出就是卡死(链接时那句 "_write is not implemented" 就是它)。 */
static void Lvgl_TraceU32(uint32_t v)
{
    char    b[10];
    uint8_t n = 0U;
    uint8_t i;

    if (v == 0U)
    {
        Usart_SendBytes((const uint8_t *)"0", 1U);
        return;
    }

    while ((v > 0U) && (n < 10U))
    {
        b[n] = (char)('0' + (v % 10U));     /* 低位先出来, 回头倒着发 */
        v /= 10U;
        n++;
    }

    for (i = 0U; i < n; i++)
    {
        Usart_SendBytes((const uint8_t *)&b[n - 1U - i], 1U);
    }
}

/* 打 8 位十六进制(看指针用) */
static void Lvgl_TraceHex(uint32_t v)
{
    static const char HEX[] = "0123456789ABCDEF";
    char    b[8];
    uint8_t i;

    for (i = 0U; i < 8U; i++)
    {
        b[i] = HEX[(v >> ((7U - i) * 4U)) & 0x0FU];
    }
    Usart_SendBytes((const uint8_t *)b, 8U);
}

/* ---- LVGL 自己的日志 -> 串口 ----
   在 Lvgl_Init() 最开头注册, 这样后面任何一步失败都能报出来。
   最有价值的一条是 lv_malloc 失败时 LVGL 自己打的
     "couldn't allocate memory (%lu bytes)" + 池子用量/碎片率 ——
   这比任何猜测都准。 */
static void Lvgl_LogCb(lv_log_level_t level, const char *buf)
{
    (void)level;

    if (s_log_cnt >= LVGL_LOG_MAX) { return; }
    s_log_cnt++;

    Lvgl_TraceStr("[LVGL] ");
    Lvgl_TraceStr(buf);
    Lvgl_TraceStr("\r\n");

    if (s_log_cnt == LVGL_LOG_MAX)
    {
        Lvgl_TraceStr("[LVGL] (日志已达上限, 后续不再打印)\r\n");
    }
}

void Lvgl_DiagMsg(const char *msg)
{
    Lvgl_TraceStr("[LVGL] ");
    Lvgl_TraceStr(msg);
    Lvgl_TraceStr("\r\n");
}

void Lvgl_DiagReport(const char *tag)
{
    lv_mem_monitor_t mon;

    lv_mem_monitor(&mon);

    Lvgl_TraceStr("[LVGL] ");
    Lvgl_TraceStr(tag);
    Lvgl_TraceStr(" used=");
    Lvgl_TraceU32((uint32_t)(mon.total_size - mon.free_size));
    Lvgl_TraceStr("/");
    Lvgl_TraceU32((uint32_t)mon.total_size);
    Lvgl_TraceStr(" max_used=");
    Lvgl_TraceU32((uint32_t)mon.max_used);
    Lvgl_TraceStr(" frag=");
    Lvgl_TraceU32((uint32_t)mon.frag_pct);
    Lvgl_TraceStr("% flush=");
    Lvgl_TraceU32(s_flush_cnt);
    Lvgl_TraceStr("\r\n");
}

#define LVGL_TRACE_STR(s)   Lvgl_TraceStr(s)
#define LVGL_TRACE_U32(v)   Lvgl_TraceU32(v)
#define LVGL_TRACE_HEX(v)   Lvgl_TraceHex(v)

#else   /* LVGL_DIAG == 0: 全部编译成空操作 */

#define LVGL_TRACE_STR(s)   do { } while (0)
#define LVGL_TRACE_U32(v)   do { } while (0)
#define LVGL_TRACE_HEX(v)   do { } while (0)

#endif /* LVGL_DIAG */

/* ==========================================================================
 * 断言失败处理
 *
 *   ⚠ **不受 LVGL_DIAG 控制**: 断言失败是错误而不是诊断信息, 任何时候
 *     都该报出来 —— 而且它往往正是"屏幕没反应"的根因。
 *     声明在 lv_conf.h 第 7 节(那边覆盖了 LVGL 默认的 while(1))。
 *
 *   为什么需要它: LVGL 的断言开关默认**全关**, 所以像
 *   lv_theme_default_init() 里"malloc 失败 -> 直接解引用 NULL"这种写法
 *   不会停在断言上, 而是 HardFault。而本工程的 HardFault_Handler 是
 *   while(1) —— 什么都不说就停住, 现象和这里一模一样, 分不清是哪个问题。
 *   这个函数的作用就是把"静默停住"变成"说清楚为什么停"。
 *
 *   套路和 App.c 里的 vApplicationStackOverflowHook 一致:
 *   打印 + 点亮 LED + 自旋。
 * ========================================================================== */
void Lvgl_AssertFail(const char *file, int line)
{
    Led_Force(1U);                  /* 常亮 = 出事了(正常状态是 1 秒一闪) */

    /* LVGL_DIAG 关掉时下面几个宏是空操作, file/line 就成了未使用参数 ——
       先显式吞掉, 免得关掉诊断反而多出两条警告 */
    (void)file;
    (void)line;

    LVGL_TRACE_STR("\r\n[LVGL] ASSERT FAIL: ");
    LVGL_TRACE_STR(file);
    LVGL_TRACE_STR(":");
    LVGL_TRACE_U32((uint32_t)line);
    LVGL_TRACE_STR("\r\n[LVGL] halted\r\n");

    for (;;) { }                    /* 停在这里等复位的信号 */
}

/* ==========================================================================
 * 画布缓冲
 *
 *   LVGL 不直接往屏幕上画 —— 它先在一块内存里把"脏了的那部分"渲染好,
 *   再调 flush 回调把这块交给屏幕。这块内存就是画布缓冲。
 *
 *   大小是个**权衡**, 不是越大越好:
 *     开全屏(128x160x2 = 40KB)  -> 一次 flush 推完整屏, 效率最高,
 *                                  但 40KB 加上 LVGL 自己的堆(40KB)就把
 *                                  128KB 的 RAM 吃掉一大半, 太紧
 *     开 40 行(10KB)            -> 一屏分 4 次 flush, 每次 128x40
 *                                  传输次数多了, 但省下 30KB RAM
 *
 *   本工程取 40 行。真要提速再把这个宏加大, 代价就是 RAM —— 改完记得
 *   看一眼链接输出的 RAM 占用。
 *
 *   ⚠ 必须是 4 的整数倍字节(LV_ATTRIBUTE_MEM_ALIGN, 见 lv_conf.h) ——
 *     LVGL 内部会按 32 位整体读写这块内存, 不对齐属于"能跑但没定义"。
 * ========================================================================== */
#define LVGL_BUF_LINES   40U
#define LVGL_BUF_BYTES   (LCD_W * LVGL_BUF_LINES * 2U)

static uint16_t s_drawbuf[LCD_W * LVGL_BUF_LINES] LV_ATTRIBUTE_MEM_ALIGN;

/* ==========================================================================
 * flush 回调: LVGL 说"这块矩形要更新", 我们把它推给屏
 *
 *   area 是**闭区间**(x2/y2 是最后一个像素, 不是长度), 所以要 +1。
 *   px_map 里的像素是 RGB565, 顺序和 ST7735S 要的一致 ——
 *   为什么不用 _SWAPPED 版本, 见 lv_conf.h 第 3 节的长注释。
 *
 *   ⚠ LCD_DrawImage 是**阻塞**的(内部 DMA 送完才返回), 所以这里紧接着
 *     调 flush_ready 是诚实的: 确实已经送完了。
 *     想改成"后台送、送完再 flush_ready"就得动 LCD 那层加完成中断回调,
 *     那是另一个话题 —— 本工程吞吐瓶颈在 SPI 的 21MHz 上, 改这个收益不大。
 *
 *   ⚠ LVGL 传进来的 px_map 在**下一次 flush 之前一直有效**, 所以这里
 *     必须在返回前把数据搬完 —— LCD_DrawImage 是同步的, 满足。
 * ========================================================================== */
static void Lvgl_FlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    s_flush_cnt++;

    /* 只打第一次。它的作用是回答一个很关键的问题:
       "到底是 LVGL 没画, 还是画了没送到屏上?" ——
       只要看到 flush#1, 就说明 LVGL 那条链是活的, 问题在屏那一侧;
       反之如果一直没有 flush#, 那就是 LVGL 根本认为屏幕不脏。 */
    if (s_flush_cnt == 1U)
    {
        LVGL_TRACE_STR("[LVGL] flush#1 ");
        LVGL_TRACE_U32((uint32_t)w);
        LVGL_TRACE_STR("x");
        LVGL_TRACE_U32((uint32_t)h);
        LVGL_TRACE_STR(" @");
        LVGL_TRACE_U32((uint32_t)area->x1);
        LVGL_TRACE_STR(",");
        LVGL_TRACE_U32((uint32_t)area->y1);
        LVGL_TRACE_STR("\r\n");
    }

    LCD_DrawImage((uint16_t)area->x1, (uint16_t)area->y1, w, h,
                  (const uint16_t *)px_map);

    lv_display_flush_ready(disp);
}

/* ==========================================================================
 * 输入: 旋转编码器 -> LVGL 的 encoder 设备
 *
 *   LVGL 会**周期性地**来调下面这个回调(周期是 LV_DEF_REFR_PERIOD, 默认 33ms),
 *   每次都期望拿到"从上次问到现在"的增量。所以这里读的是
 *   Encoder_ReadDelta() —— 它正好是"读走并清零"的语义, 天生对得上。
 *
 *   ⚠ 用 Encoder_ReadDelta() 而不是 Encoder_GetCW/GetCCW:
 *     后两个是**累计值**, 得自己记住上次读到哪, 少记一次就丢一格。
 *
 *   ⚠ 用 Encoder_SwIsDown() 而不是 Encoder_SwTakePress():
 *     LVGL 要的是**电平**(按着还是松着), 边沿由 LVGL 自己判 —— 它靠这个
 *     实现长按、连按这些行为。给它边沿事件的话, 长按就永远不成立。
 * ========================================================================== */
static lv_group_t *s_grp   = NULL;
static lv_indev_t *s_indev = NULL;

static void Lvgl_EncReadCb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    data->enc_diff = (int16_t)Encoder_ReadDelta();

    data->state = (Encoder_SwIsDown() != 0U) ? LV_INDEV_STATE_PRESSED
                                             : LV_INDEV_STATE_RELEASED;
}

/* ==========================================================================
 * 初始化: 内核 + 时基 + 显示 + 输入
 * ========================================================================== */
void Lvgl_Init(void)
{
    lv_display_t *disp;

    /* 日志回调**必须最先注册** —— 它要能抓到后面每一步的失败。
       注册之前 LVGL 的日志是没人接的(默认 printf 那条路在本工程是死的)。 */
#if (LVGL_DIAG != 0)
    lv_log_register_print_cb(Lvgl_LogCb);
#endif

    LVGL_TRACE_STR("\r\n[LVGL] start\r\n");

    /* ---- 1. 内核 ----
       只会初始化一次(内部有守卫), 分配内存池。 */
    lv_init();

    Lvgl_DiagReport("init ok");

    /* ---- 2. 时基 ----
       ⚠ 必须放在任何 lv_timer / 动画之前。Tick_GetMs 底层是
         xTaskGetTickCount(), 所以本函数只能在调度器起来之后调。 */
    lv_tick_set_cb(Tick_GetMs);

    /* ---- 3. 显示 ----
       ⚠ 这里分配的东西最多: 显示设备本身 + 画布层 + 刷新定时器 +
         **整个默认主题**(43 个 style, 每个还要挂一串属性)。
         内存池不够的话就是死在这一步 —— 见 lv_conf.h 第 1 节的说明。

       第一个创建的显示设备会自动成为默认显示, 控件默认往它上面挂。 */
    LVGL_TRACE_STR("[LVGL] display create...\r\n");
    disp = lv_display_create(LCD_W, LCD_H);

    LVGL_TRACE_STR("[LVGL] disp=0x");
    LVGL_TRACE_HEX((uint32_t)disp);
    LVGL_TRACE_STR("\r\n");

    /* ⚠ 必须挡住 NULL。不挡的话后面每个 lv_xxx() 都会被 LV_CHECK_ARG
       静默挡掉(它只记日志、直接 return), 现象是**什么都没画但也不报错**,
       而 lv_display_set_buffers(NULL, ...) 之后再调 lv_screen_active()
       还会拿到 NULL —— 一路静默到底, 极难定位。 */
    if (disp == NULL)
    {
        Lvgl_DiagMsg("display create FAILED -- 放弃后续初始化");
        return;
    }

    lv_display_set_flush_cb(disp, Lvgl_FlushCb);
    lv_display_set_buffers(disp, s_drawbuf, NULL, LVGL_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    Lvgl_DiagReport("display ok");

    /* ---- 4. 输入: 旋转编码器 ----
       LVGL 的 encoder 设备类型天生就是为本工程这种"转 + 按"的器件设计的:
         转 -> 在控件之间移动焦点(焦点在控件自己身上, 不用我们管)
         按 -> 把 LV_KEY_ENTER 送给当前焦点控件(按钮就当点了一下)
       我们只负责在它每次来问的时候报两个数: 转了几格、现在按没按着。

       ⚠ 焦点分组是必须的: 没有分组的话 encoder 无处可移, 转动完全没反应。 */
    s_grp = lv_group_create();
    lv_group_set_default(s_grp);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_indev, Lvgl_EncReadCb);
    lv_indev_set_group(s_indev, s_grp);

    Lvgl_DiagReport("indev ok");
}

/* ==========================================================================
 * 主心跳
 * ========================================================================== */
void Lvgl_TaskHandler(void)
{
    lv_timer_handler();
}
