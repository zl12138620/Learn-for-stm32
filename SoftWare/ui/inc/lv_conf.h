/**
  ******************************************************************************
  * @file    SoftWare/ui/inc/lv_conf.h
  * @brief   LVGL v9.6.0 配置 —— LVGL 唯一的配置入口
  *
  *          ⚠ 这个文件**故意写得很短**: 只列本工程**主动改过**的项, 其余
  *            几百个选项全部走 LVGL 内置默认值(默认值在
  *            LVGL/include/lvgl/config/lv_conf_internal.h)。
  *
  *          为什么不照抄官方的 lv_conf_template.h(2640 行):
  *            那份文件是把**所有默认值摊开写出来给你看**的。抄进来的话,
  *            一是以后升级 LVGL 要跟着重新 merge 一遍, 二是真正改过的那
  *            几行会被埋在 2600 行里找不着 —— 和本工程"看一眼就知道这块
  *            板子跑了些什么"的风格正好相反。
  *            要查某个选项的默认值和可选项, 看 LVGL/lv_conf_template.h
  *            (已随源码一起放进仓库, 没删)。
  *
  *          怎么被找到的:
  *            CMakeLists.txt 里定义了 LV_CONF_INCLUDE_SIMPLE,
  *            lv_conf_internal.h 见到这个宏就会 #include "lv_conf.h" 走
  *            include 路径去找。本文件在 ui 领域的 inc/ 里, 已经在搜索路径上。
  *            (官方 lv_conf_template.h 开头写的就是这条路:
  *             "define LV_CONF_INCLUDE_SIMPLE; add the path as an include path")
  *
  *          ⚠ 必须定义 LV_CONF_H —— lv_conf_internal.h 靠它判断配置到底有
  *            没有真的被读到。没读到只会给一句很容易漏看的 #pragma message,
  *            然后**静默地全用默认值**(LV_MEM_SIZE 会变成 64KB, 直接爆 RAM)。
  ******************************************************************************
  */

#ifndef LV_CONF_H
#define LV_CONF_H

/* ==========================================================================
 * 1. 内存 —— 必须改, 默认值(64KB)放不下
 *
 *    LVGL 自己带一个静态堆(控件、样式、动画对象都从这儿分配), 大小在编译期
 *    就定死成 .bss 里的一块数组。
 *    官方默认 64KB —— 本芯片一共 128KB RAM, 去掉栈和画布缓冲只剩约 70KB,
 *    照默认值来直接就满了。
 *
 *    ---- 16KB 是怎么定下来的 (2026-09-22 实测) ----
 *    ⚠ 这一段记着两次判断失误, 别再犯第三次:
 *
 *    第一次: 我写"演示页那几个控件 + 默认主题, 10KB 量级" ——
 *      那个"10KB"是**估的, 我却写成了好像量过**。事实是当时没有任何数据。
 *    第二次: 第一次烧录后"屏幕没反应", 我按"内存不够"处理, 一口气提到 40KB。
 *      **方向搞错了** —— 真正的原因是 UiTask 栈溢出(见 App.c 的 UI_TASK_STACK),
 *      和内存池一点关系都没有。40KB 白占 24KB RAM, 用户当场就问"RAM 怎么这么多"。
 *
 *    实测数据(串口 Lvgl_DiagReport 的输出, 板上跑出来的, 不是算的):
 *        init ok    used=2080
 *        display ok used=6068      <- 默认主题(43 个 style)一共只要约 4KB
 *        indev ok   used=6480
 *        demo built used=8512      <- 六个控件再加 2KB
 *    所以 24KB 本来就是够的。现在给 16KB, 对实测峰值 8.5KB 约 1.9 倍余量。
 *
 *    ⚠ 要调这个值, **先看串口** Lvgl_DiagReport() 打出的 used/max_used,
 *      按实际值给 2 倍, 不要凭感觉。max_used 是历史峰值, 比 used 更有参考价值。
 *
 *    ---- 又提到 24KB (2026-09-22 正式界面) ----
 *    ⚠ 这一次要说清楚**它还不是一个诊断结论, 只是保险**:
 *      演示页(6 个控件)的峰值是 8.5KB, 换成正式的三个界面(约 30 个控件 +
 *      3 个屏幕对象)之后估算在 12~14KB, 离 16KB 太近。
 *      而池子不够的后果是**静默 NULL 解引用 -> HardFault -> 整机无声死掉**
 *      (见第 7 节), 所以宁可先给宽 —— 这一步的目的是把"可能无声死掉"
 *      变成"要么跑起来、要么留下可读的日志"。
 *      真的消耗量由 App.c 里那几个 Lvgl_DiagReport 打出来, 拿到数再往回收。
 *      (这一条是上次的教训: 当时也是直接把池子加大, 但**没有同时加测量**,
 *       结果方向错了还白占 24KB RAM, 见上面那段。)
 *
 *    ⚠ 另外记住 lv_malloc 失败**不会报错**(断言默认全关, 见第 7 节):
 *      lv_theme_default_init() 里分配失败会直接解引用 NULL -> HardFault,
 *      而 HardFault_Handler 是 while(1), 整机静默死掉。
 *      所以第 5 节把日志接到串口是必要的 —— 失败时会打
 *      "couldn't allocate memory (N bytes)", 那才是唯一可靠的信源。
 * ========================================================================== */
#define LV_MEM_SIZE (24U * 1024U)

/* ==========================================================================
 * 2. 操作系统对接 —— 本工程刻意选 NONE
 *
 *    本工程**确实**在跑 FreeRTOS(v11.3.1), 但这里仍然选 LV_OS_NONE, 原因:
 *      a) 只有 UiTask 一个任务碰 LVGL, LVGL 内部那把大锁没有意义;
 *      b) 选 FREERTOS 会让 LVGL 反过来 #include FreeRTOS.h、依赖内核的
 *         task notification —— LVGL 内核就得知道本工程的内核配置,
 *         第三方库不该有这层耦合;
 *      c) 真需要 LVGL 自己线程安全时, 用 LV_OS_CUSTOM + 本工程的互斥量
 *         更可控(那时要注意 UiTask 的栈够不够, LVGL 的绘制是在调用者栈上跑)。
 *
 *    ⚠ 选 NONE 就意味着**所有 lv_xxx() 调用必须在同一个任务里**。
 *      目前是 UiTask。别的任务要碰 LVGL, 必须自己加锁并保证只在这一个任务里调。
 * ========================================================================== */
#define LV_USE_OS LV_OS_NONE

/* ==========================================================================
 * 3. 颜色格式 —— RGB565, 和 ST7735S 一致
 *
 *    ⚠ 名字注意: v9.6 里 LV_COLOR_DEPTH 已经废弃, 换成了
 *      LV_COLOR_FORMAT_DEFAULT(用旧名字会吃一个 deprecation 警告)。
 *
 *    ⚠ 确定不用 _SWAPPED 版本(这是移植时最容易翻车的地方, 结论是**不需要**):
 *      LV_COLOR_FORMAT_RGB565        内存里是小端 native 顺序
 *      LV_COLOR_FORMAT_RGB565_SWAPPED 把两个字节调过来
 *      该用哪个**只取决于屏幕那头是怎么收字节的**, 和 LVGL 无关。
 *
 *      本工程走的是: LCD_DrawImage -> DMA(两侧都是 HalfWord) -> SPI 配成
 *      SPI_DataSize_16b。SPI 移位寄存器是**高字节先出**, 所以内存里那个
 *      little-endian 的 0xF800 送到线上正好是 F8 00 —— 正是 ST7735S 要的顺序。
 *
 *      旁证: 摄像头画面走的是同一个 LCD_DrawImage, 颜色一直是对的。
 *
 *      如果哪天改成按字节(8 位)往外推像素, 就必须换成 _SWAPPED。
 * ========================================================================== */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/* ==========================================================================
 * 4. 缓冲区对齐
 *
 *    lv_conf_internal.h 里这个宏的默认值是**空的**, 但 LVGL 要求传给
 *    lv_display_set_buffers() 的画布缓冲按 4 字节对齐 —— 它内部会按
 *    32 位整体读写那块内存。不对齐在 Cortex-M4 上不一定立刻炸,
 *    但属于"能跑但没定义"的行为。
 *
 *    官方文档推荐的写法就是这个, 直接照抄。
 * ========================================================================== */
#define LV_ATTRIBUTE_MEM_ALIGN __attribute__((aligned(4)))

/* ==========================================================================
 * 5. 日志 —— **打开, 并且接到串口**
 *
 *    2026-09-22 第一次烧录排查"什么都没有"时打开的。它是最直接的证据来源:
 *    lv_malloc 失败时 LVGL 自己会打
 *      "couldn't allocate memory (%lu bytes)" + 池子用量/碎片率,
 *    比任何猜测都准。
 *
 *    ⚠ 光把 LV_USE_LOG 改成 1 是**不够的**: LVGL 默认走 printf ->
 *      newlib-nano 的 _write -> 本工程没实现(链接时那句
 *      "_write is not implemented and will always fail" 就是它),
 *      日志会石沉大海。必须:
 *        LV_LOG_PRINTF 0                 明确不走 printf
 *        lv_log_register_print_cb(...)   自己注册回调转投到串口
 *      回调实现在 Lvgl_Port.c 的 Lvgl_LogCb, 在 Lvgl_Init() 最开头注册。
 *
 *    ⚠ 级别设成 INFO 而不是默认的 WARN: 上面那条 "couldn't allocate memory"
 *      是 **INFO 级**的。只开 WARN 的话, 内存不足这种最要命的失败反而看不见。
 *
 *    ⚠ 回调里加了 20 条的条数上限(Lvgl_Port.c 的 LVGL_LOG_MAX): 串口只有
 *      9600, 一个字节约 1ms, 真让它在循环里刷日志, 光打印就能把系统拖成
 *      "看起来卡死", 反而掩盖真正的问题。
 * ========================================================================== */
#define LV_USE_LOG   1
#define LV_LOG_LEVEL LV_LOG_LEVEL_INFO
#define LV_LOG_PRINTF 0

/* ==========================================================================
 * 6. 主题 —— 只留 default
 *
 *    本工程用默认主题(控件有像样的圆角/阴影/配色)。另外两个主题是给
 *    单色屏和极简场景的, 留着纯属浪费 flash。
 * ========================================================================== */
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO   0

/* ==========================================================================
 * 7. 断言失败时做什么 —— 打印后停, 而不是默认那个静默的 while(1)
 *
 *    ⚠ 先看清楚一件事: **LVGL 的断言默认是全关的**
 *      (LV_USE_ASSERT_MALLOC / _NULL / _OBJ / _STYLE 在 lv_conf_internal.h
 *       里的默认值都是 0)。
 *      所以像 lv_theme_default_init() 里那种
 *          theme_def = lv_malloc_zeroed(...);
 *          LV_ASSERT_MALLOC(theme_def);   <-- 空操作
 *          ... theme->inited ...          <-- 拿 NULL 解引用
 *      的写法不会停在断言上, 而是直接 HardFault。这就是第一次烧录
 *      "屏幕没变化 + LED 也不正常"的成因。
 *
 *    lv_assert.h 里是 #ifndef 保护的, 所以在这里定义就能覆盖默认的
 *    "while(1);"。默认那个什么都不说就停住, 现象和 HardFault 一模一样,
 *    根本分不清是谁的问题。
 *
 *    实现(Lvgl_Port.c 的 Lvgl_AssertFail)和 vApplicationStackOverflowHook
 *    一个套路: 打印 + 点亮 LED + 自旋, 让"卡住"在串口和灯上都看得见。
 *
 *    ⚠ 包在 #ifndef __ASSEMBLY__ 里: lv_conf.h 在汇编上下文也会被包含
 *      (lvgl_public.h 的 __ASSEMBLY__ 分支), 而函数声明不是合法汇编。
 * ========================================================================== */
#ifndef __ASSEMBLY__
void Lvgl_AssertFail(const char *file, int line);
#define LV_ASSERT_HANDLER do { Lvgl_AssertFail(__FILE__, __LINE__); } while (0)
#endif

/* ==========================================================================
 * 8. 中文字体
 *
 *    lv_font_simhei_16.c 由 `python 工具/gen_lvgl_font.py` 生成(黑体 16px,
 *    30 个界面用字 + 可打印 ASCII)。那个脚本**顺便会打两个补丁**,
 *    直接跑 npx lv_font_conv 得到的文件是不全的 —— 见脚本开头的说明。
 *
 *    ⚠⚠ 把默认字体换成自定义字体, 不能用 `#define LV_FONT_DEFAULT LV_FONT_SIMHEI_16`
 *       这种写法。lv_conf_internal.h 里那批 `LV_FONT_DEFAULT_MONTSERRAT_xx`
 *       是**内置字体专用**的宏; 自定义字体要的是**取地址** `&lv_font_xxx`。
 *       正确入口是 LV_USE_CUSTOM_FONT_DEFAULT:
 *         LV_USE_CUSTOM_FONT_DEFAULT = 1 时, 内置字体那一段选择逻辑会被整个隐藏,
 *         此时**必须**自己定义 LV_FONT_DEFAULT, 否则会链到没编进来的字体上。
 *       (模板里写得很清楚: "If you do not, LV_FONT_DEFAULT falls back to a
 *        builtin font, which is no longer compiled in, causing a link error.")
 *
 *    ⚠ LV_FONT_CUSTOM_DECLARE 是给字体做 extern 声明的, 内容在
 *      lv_font.h 里被展开。必须写, 不写的话 LV_FONT_DEFAULT 用到的符号
 *      在部分编译单元里没有声明。
 *
 *    为什么把默认字体整个换掉: text_font 是**可继承样式属性**,
 *    主题会把它写进 styles.scr —— 屏幕上所有没自己指定字体的文字都会用它,
 *    不用逐个控件设。换这一处等于换全部。
 * ========================================================================== */
#define LV_FONT_SIMHEI_16              1
/* 28px 那套只有数字和空格, 给舵机界面那个大角度值用(设计上它是那一屏
   唯一的主角)。11 个字形, 几 KB flash。
   ⚠ 别往里加汉字 —— 28px 的中文在 128 宽的屏上一行放不下几个。 */
#define LV_FONT_SIMHEI_28_NUM          1
#define LV_USE_CUSTOM_FONT_DEFAULT     1
#define LV_FONT_CUSTOM_DECLARE         LV_FONT_DECLARE(lv_font_simhei_16)
#define LV_FONT_DEFAULT                &lv_font_simhei_16

/* Montserrat 14 换成上面那套之后就没人用了, 关掉省约 25KB flash。
   ⚠ 顺序要紧: LV_FONT_DEFAULT 必须先指到 simhei, 否则主题会拿到一个
     指向"没编进来的字体"的指针, 编译能过、链接才发现。 */
#define LV_FONT_MONTSERRAT_14          0

/* ==========================================================================
 * 9. 主题过渡动画
 *
 *    默认 80ms。128x160 上这个动画没什么意义, 反而每次移焦点都多几帧重绘。
 *    设 0 直接关掉, 手感更"跟手"。
 * ========================================================================== */
#define LV_THEME_DEFAULT_TRANSITION_TIME 0

#endif /* LV_CONF_H */
