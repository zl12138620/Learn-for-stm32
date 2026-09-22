# FreeRTOS 与 LVGL 移植笔记

> 记于 2026-09-22。板子是**立创·天空星 STM32F407VGT6**，工具链是 CMake + arm-none-eabi GCC + Ninja，
> 跑的是 ST 标准外设库（不是 HAL）。
>
> 这份文档写给"准备在自己板子上做同样两件事的人"——也就是几个月后忘了细节的我自己。
> 所以**踩坑那一章写得比"正确做法"那一章长**，而且会把**当时的错误判断**一起记下来。
> 最终答案现在网上一搜就有，但"为什么会往错的方向修"才是真正会重复发生的东西。

---

## 0. 总览

| | FreeRTOS | LVGL |
|---|---|---|
| 版本 | v11.3.1 | v9.6.0 |
| 内核放哪 | 顶层 `FreeRTOS-Kernel/`，不改 | 顶层 `LVGL/`，不改 |
| 配置放哪 | `SoftWare/system/inc/FreeRTOSConfig.h` | `SoftWare/ui/inc/lv_conf.h` |
| 对接代码 | 分散（`Tick.c` 的时基、`Encoder.c` 的临界区） | 集中（`SoftWare/ui/src/Lvgl_Port.c`） |
| 移植耗时感受 | 一天 | 断断续续好几天，**大部分时间花在排查上** |

**为什么 FreeRTOS 在前**：LVGL 的时基可以直接借 FreeRTOS 的 tick，任务模型也让"主循环被
摄像头抓帧占住 95ms"这个问题先解决了 —— 否则 LVGL 的 `lv_timer_handler()` 会被摄像头饿死，
你会在 LVGL 里排查一个其实属于任务划分的问题。

**两次移植的共同套路**（这个套路值得记住）：

> 第三方库的内核**原封不动**放在顶层目录，配置和对接代码放在自己的工程结构里。
> 好处是升级库的时候只需要替换顶层那个目录，自己的代码一行不用动；
> 而且 `git diff` 顶层目录永远是干净的，"我到底改没改过库"这个问题不用猜。

---

## 1. FreeRTOS 移植

### 1.1 文件布局

```
FreeRTOS-Kernel/                        ← 顶层，原封不动
├─ *.c                                  ← tasks.c queue.c list.c timers.c ...
├─ include/
└─ portable/
    ├─ GCC/ARM_CM4F/port.c  portmacro.h ← Cortex-M4F 的端口层
    └─ MemMang/heap_4.c                 ← 堆实现，见 1.4
```

`CMakeLists.txt` 里把 `FreeRTOS-Kernel/include` 和 `portable/GCC/ARM_CM4F` 加为
`SYSTEM` include（抑制库头内部的宏重定义噪声），源文件收集用的是**精确路径**，
不是 `GLOB` —— 因为 `portable/` 下面有一堆别的编译器的端口层（IAR、Keil、RVDS……），
`GLOB` 会把它们全收进来，然后在编译时报几千行错。

### 1.2 ⚠ 最大的坑：`NVIC_PriorityGroupConfig` 必须最先调

**这个坑的隐蔽程度是这次移植之最，因为它完全不报错。**

现象：源码里明明写了"EXTI 抢占优先级 4、TIM7 抢占优先级 5"，注释里还煞有介事地
推导"EXTI 能抢占 TIM7，所以顺序要小心"，但**实际行为是三个中断完全不能互相抢占**。

根因：StdPeriph 的 `NVIC_Init()` 是**读 `SCB->AIRCR` 的 PRIGROUP 字段现算**寄存器值的：

```c
/* stm32f4xx_misc.c 里的大意 */
tmppriority = (0x700 - ((SCB->AIRCR) & (uint32_t)0x700)) >> 0x08;
tmppre      = (0x4 - tmppriority);
tmpgroup    = ...;
tmppriority = NVIC_IRQChannelPreemptionPriority << tmppre;   /* ← 左移的位数由 PRIGROUP 决定 */
```

F407 的 `AIRCR.PRIGROUP` **复位值是 0**，对应 `NVIC_PriorityGroup_0`，也就是
"0 位抢占 + 4 位子优先级"。在这个模式下，你传进去的 `NVIC_IRQChannelPreemptionPriority`
**移位后全部落进了子优先级字段**，抢占优先级恒为 0。

后果：
- 所有中断的**抢占优先级都是 0**，互相之间谁也不能打断谁
- **没有任何报错、警告或断言** —— 程序照跑，只是在你以为会发生抢占的地方不会发生

解法（`USER/main.c`，必须在**任何** `NVIC_Init` / 中断使能之前）：

```c
int main(void)
{
    /* ⚠ FreeRTOS 要求 4 位全给抢占优先级。放在所有外设初始化之前。 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    Led_Init();
    Usart_Init(9600);
    ...
}
```

**为什么 FreeRTOS 偏偏要求 `PriorityGroup_4`**：FreeRTOS 的临界区靠
`BASEPRI` 屏蔽"优先级低于阈值"的中断。如果优先级里混了"子优先级"的语义，
`BASEPRI` 的比较就失去意义 —— 内核没法用一个数值判断"这个中断该不该被屏蔽"。
所以 FreeRTOS 的硬性要求是：**4 位全是抢占优先级，不带子优先级**。

**教训**：这是典型的"配置项看着无害、错了也静默"的坑。第一次遇到时我是在审计
中断优先级注释时发现对不上的——注释说的行为和实际不可能发生的行为不一致。
**注释和代码行为对不上时，先怀疑配置，不要先怀疑注释写错了。**

### 1.3 中断优先级与 `...FromISR()`

```c
#define configPRIO_BITS                              4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5
```

规则：**任何会调用 `xxxFromISR()` 的中断，抢占优先级数值必须 ≥ 5**。

为什么是"数值 ≥"而不是"优先级 ≥"：Cortex-M 里**数值越小优先级越高**。
`MAX_SYSCALL` 是"内核允许被 API 影响的最低优先级"（数值上界），
比它**数值更小**（优先级更高）的中断不受内核临界区保护，
在里面调 FreeRTOS API 会命中 `vPortValidateInterruptPriority()` 的断言。

本工程把这个用在了编码器上：`EXTI9_5` 和 `TIM7` 都是 5。它们当时其实**并没有**调用
任何 FreeRTOS API（只是改几个 `volatile` 变量、读写寄存器），但设成合规值之后，
以后想加就随时能加，不用回头重新推导一遍。

### 1.4 堆：`heap_4`

`heap_4` 相对 `heap_1`（只能分配不能释放）和 `heap_2`（能释放但不合并）的差别是
**会合并相邻空闲块**，所以适合会反复申请释放的场景。LVGL 进来之后这一点很重要 ——
LVGL 的控件是动态建的，用 `heap_2` 会因为碎片慢慢耗死。

大小：`configTOTAL_HEAP_SIZE`。**这个值的确定方式只能是量，不能估**，见第 4 章。

### 1.5 SysTick 归内核之后，原来的时基怎么办

移植前 `Tick.c` 自己写 `SysTick_Handler`、自己 `SysTick_Config`、自己累加 `s_tick_ms`。
移植后 SysTick 归内核了，所以 `Tick.c` 变成一层薄包装：

```c
uint32_t Tick_GetMs(void)
{
    return (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);
}

void vApplicationTickHook(void)      /* configUSE_TICK_HOOK = 1 */
{
    if (s_ms_cb) { s_ms_cb(); }
}
```

**这么包一层的好处是上层的代码一行都不用改** —— `Led.c` / `OV7670.c` / `App.c`
原来怎么调 `Tick_GetMs()` 现在还怎么调。

**1ms 采样为什么要挂 tick 钩子，而不是放在任务里轮询**：
编码器的按键消抖必须每毫秒采一次。摄像头界面一轮约 95ms（等一帧 + 读 FIFO + 送屏），
任务级轮询会直接漏掉一次快速的点按。而且**判电平也不行** —— 一次按下在随后每一轮
都会被判成"按下"，现象是"进功能界面后立刻又弹回主菜单"。
所以采样必须在 1ms 的硬节拍上做，任务只负责取"按下事件"。

⚠ 钩子里的约束：**在中断上下文执行**，只能做很快的活（读个引脚、置个标志）。
不许阻塞、不许调没带 `FromISR` 后缀的 API、也不要在这里调 `Tick_GetMs()`
（tick 计数在钩子执行期间还没递增）。

### 1.6 分层没被破坏：用回调反转，别直接 include

`SoftWare/` 是按功能领域分层的（`system/` `comms/` `display/` `camera/` `motion/` `app/`），
规则是**只能往下依赖，同层之间不互相依赖**。

移植 FreeRTOS 时踩了一个：`Tick.c`（在 `system/`）里 `#include "Encoder.h"`（在 `motion/`），
于是 `system/ -> motion/`，**底层依赖了上层**，`system/` 也不再自包含、不能整块搬走了。

解法是**反转依赖方向**：让 `Tick` 提供一个回调注册口，由 `Encoder_Init()` 主动注册自己。

```c
/* system/inc/Tick.h —— Tick 完全不认识任何人 */
void Tick_SetMsCallback(void (*fn)(void));

/* motion/src/Encoder.c —— 上层主动注册 */
void Encoder_Init(void)
{
    ...
    Tick_SetMsCallback(Encoder_SwTick1ms);
}
```

代价是 28 字节 FLASH + 4 字节 RAM。**要回调就用这种反转，别图省事直接 include。**

### 1.7 ⚠ FreeRTOS 唯一的"例外条款"

移植后，`system/`（Tick）、`motion/`（编码器临界区）、`display/`（LCD 互斥量）、`app/`
都会 include FreeRTOS 的头文件。这**不算破坏分层** —— FreeRTOS 是第三方库，
不是本工程的领域，规则管的是"领域之间"的关系。

代价是这几个领域不再能"整块搬到没有 FreeRTOS 的工程"了。真要搬得把用到的
RTOS 原语换掉（临界区、互斥量、任务通知各一处，量不大）。

### 1.8 中断处理函数放哪

**跟着它的驱动模块走**，不要集中到 `System/stm32f4xx_it.c`：

| 中断 | 位置 |
|---|---|
| `EXTI9_5` / `TIM7` | `motion/src/Encoder.c` |
| `SysTick` | 由内核的 `xPortSysTickHandler` 转发到 `Tick.c` 的钩子 |
| `USART1` | `comms/src/Usart.c` |
| 内核异常（HardFault 等） | `System/stm32f4xx_it.c`（只剩这些） |

好处是**中断和它读写的变量在同一个文件里**，想查"谁改了这个变量"不用跳文件。

⚠ 一个中断向量只能定义一次 —— 在 `it.c` 里重新定义会报
`multiple definition of ..._IRQHandler`。

### 1.9 三个钩子值得全开

```c
#define configUSE_MALLOC_FAILED_HOOK      1
#define configCHECK_FOR_STACK_OVERFLOW    2
```

为什么值得：**初学 RTOS 最常见的翻车是"系统莫名卡死"**，而栈给小了、堆不够导致
建任务失败这两种情况，**默认都是不报错的**，只表现为"某个任务再也不跑"。
开了钩子会当场停在明处。

本工程的实现（`App.c`）：打印 + 点亮 LED + 自旋。第 4 章会看到，这个钩子
**真的救了一次命**。

---

## 2. LVGL 移植

### 2.1 ⚠ 必须是 v9，不能用 v8

网上大量教程还是 v8 的，**两者 API 不兼容**。几个关键差异：

| v8 | v9 |
|---|---|
| `lv_disp_drv_t` + `lv_disp_drv_register()` | `lv_display_t` + `lv_display_create()` |
| 缓冲区大小按**像素**算 | 按**字节**算 |
| `lv_tick_inc()` 要自己周期性调 | `lv_tick_set_cb()` 注册回调 |
| `lv_scr_act()` | `lv_screen_active()` |
| `lv_obj_clear_flag()` | `lv_obj_remove_flag()` |
| `lv_btn` | `lv_button` |
| `LV_COLOR_DEPTH` | `LV_COLOR_FORMAT_DEFAULT`（v9.6 起 `LV_COLOR_DEPTH` 废弃） |

### 2.2 v9.6 的头文件布局（和你想的不一样）

```
LVGL/
├─ lvgl.h                  ← 只干一件事: #include "include/lvgl/lvgl.h"
├─ include/lvgl/           ← **真头文件在这里**
│   ├─ lvgl.h              ← 聚合头, 内部彼此全是相对包含("core/lv_obj.h")
│   ├─ config/lv_conf_internal.h
│   └─ core/ display/ widgets/ font/ ...
└─ src/                    ← 实现 + 私有头
    ├─ core/lv_obj.c ...
    └─ core/lv_obj_private.h
```

⚠ **`src/` 下那些 `.h` 很多是废弃壳**，内容只有一句
`#warning ... deprecated ... #include "..."`。查 API 要去 `include/lvgl/` 那份。

**include 路径只需要一个**：`LVGL/` 根目录。因为内部包含全走相对路径
（`src/core/lv_obj.c` 里是 `#include "../include/lvgl/lvgl.h"`）。
自己代码写 `#include "lvgl.h"` 即可。

### 2.3 `LV_CONF_INCLUDE_SIMPLE` —— 不定义的后果是**静默**

官方 `lv_conf_template.h` 开头写得很清楚：

```
Copy this file as `lv_conf.h`
1. simply next to `lvgl` folder
2. or to any other place and
   - define `LV_CONF_INCLUDE_SIMPLE`;
   - add the path as an include path.
```

本工程走第 2 条（配置放 `SoftWare/ui/inc/`），所以：

```cmake
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
```

作用在 `lv_conf_internal.h` 的查找逻辑里：

```c
#ifdef LV_CONF_PATH                      /* 指定路径 */
#elif defined(LV_CONF_INCLUDE_SIMPLE)    /* 走 include 路径找 "lv_conf.h" ← 我们 */
    #include "lv_conf.h"
#else
    #include "../../../../lv_conf.h"     /* 假设配置在 lvgl 目录旁边 */
#endif
```

⚠ **v9.6 加了 `__has_include` 自动探测，所以不定义多半也能编译过** —— 但失败时
只给一句极易漏看的 `#pragma message`，然后**静默使用全默认值**。
最直接的后果是 `LV_MEM_SIZE` 变成默认的 64KB，在 128KB RAM 的板子上直接爆掉。

**判断配置到底有没有被读到**：查符号表，LVGL 的堆是一块静态数组。

```bash
arm-none-eabi-nm -S --size-sort -r build/output/xxx.elf | head -6
# 2001028c 00006000 b work_mem_int.0     ← 0x6000 = 24576 = LV_MEM_SIZE，说明配置生效了
```

### 2.4 颜色格式：RGB565 还是 RGB565_SWAPPED

**这个只能看"屏幕那头怎么收字节"，和 LVGL 无关。** 判断方法：

1. 找到像素最终是怎么送出去的
2. 如果 SPI 是**16 位数据宽度**（`SPI_DataSize_16b`）+ DMA 两侧都是 `HalfWord`
   → 移位寄存器**高字节先出** → 内存里 little-endian 的 `0xF800` 送到线上正好是 `F8 00`
   → **用 native `LV_COLOR_FORMAT_RGB565`**
3. 如果是按**字节**（8 位）往外推像素 → 必须用 `_SWAPPED`

本工程的旁证：摄像头画面走的是同一个 `LCD_DrawImage()`，颜色一直是对的。

⚠ 这个判断错了的现象是"颜色不对"（红蓝互换之类），**不是"没有画面"**。
排查"没画面"时不要往这个方向想。

### 2.5 `LV_USE_OS = LV_OS_NONE` —— 明明是 RTOS 工程却选 NONE

理由：
1. 只有 UiTask 一个任务碰 LVGL，LVGL 内部那把大锁没有意义
2. 选 `LV_OS_FREERTOS` 会让 LVGL 反过来 `#include FreeRTOS.h`、依赖内核的
   task notification —— **第三方库不该有这层耦合**
3. 真需要线程安全时，用 `LV_OS_CUSTOM` + 自己的互斥量更可控

**代价（必须记住）**：LVGL **没有任何内部锁**，所以
**所有 `lv_xxx()` 调用必须在同一个任务里**。

这条约束会一路影响你的架构设计。本工程的例子：摄像头抓帧在 `CameraTask`（优先级 2），
但画面渲染必须回到 UiTask —— 所以 `CameraTask` 一个 `lv_*` 都不能调，
只能填数据 + 推一个序号，由 UiTask 去 `lv_obj_invalidate()`。

### 2.6 移植层就三个对接点

```c
void Lvgl_Init(void)
{
    lv_init();
    lv_tick_set_cb(Tick_GetMs);              /* ① 时基: 现成的直接用 */

    lv_display_t *d = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(d, Lvgl_FlushCb); /* ② 显示: 见下 */
    lv_display_set_buffers(d, s_drawbuf, NULL, sizeof(s_drawbuf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ③ 输入: 编码器接到 LVGL 的 encoder 设备类型上 */
    s_grp = lv_group_create();
    lv_group_set_default(s_grp);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_indev, Lvgl_EncReadCb);
    lv_indev_set_group(s_indev, s_grp);
}
```

**flush 回调白捡一个驱动**：现有 `LCD_DrawImage(x,y,w,h,buf)` 的签名和
flush 回调要的东西**正好对上**，而且它内部就是 DMA 送 SPI。

```c
static void Lvgl_FlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);   /* ⚠ x2/y2 是闭区间, 要 +1 */
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    LCD_DrawImage((uint16_t)area->x1, (uint16_t)area->y1, w, h,
                  (const uint16_t *)px_map);

    lv_display_flush_ready(disp);
}
```

⚠ **画布缓冲对着色器要求 4 字节对齐**（`LV_ATTRIBUTE_MEM_ALIGN` 在
`lv_conf_internal.h` 里默认是**空的**，要自己在 `lv_conf.h` 定义成
`__attribute__((aligned(4)))`）—— LVGL 内部会按 32 位整体读写那块内存。

### 2.7 `lv_conf.h` 应该写多少

官方 `lv_conf_template.h` 有 **2640 行**，是把所有默认值摊开给你看的。

**建议只写"主动改过的"那几项**，理由：
- 抄进来之后，以后升级 LVGL 要跟着重新 merge 一遍
- 真正改过的那几行会被埋在 2600 行里找不着

要查某个选项的默认值和可选项，看 `LVGL/lv_conf_template.h`（跟着源码一起进仓库）。
默认值本身在 `LVGL/include/lvgl/config/lv_conf_internal.h`。

⚠ **一个必须注意的细节**：`lv_conf.h` **必须定义 `LV_CONF_H`**。
`lv_conf_internal.h` 靠它判断配置到底有没有被读到，没读到只会给一句
`#pragma message`，然后静默全用默认值。

### 2.8 本工程的 `LV_MEM_SIZE` 是怎么定下来的

**只有一条路：量。** 用 `lv_mem_monitor()`：

```c
lv_mem_monitor_t mon;
lv_mem_monitor(&mon);
/* mon.total_size / mon.free_size / mon.max_used / mon.frag_pct */
```

`max_used` 是**历史峰值**，比 `used` 有参考价值得多。

实测记录（128×160、默认主题、六个控件的演示页）：

```
init ok     used=2080      ← 内核自己
display ok  used=6068      ← +3988，几乎全是默认主题的 43 个 style
indev ok    used=6480      ← +412
demo built  used=8512      ← +2032，六个控件 ≈ 340 B/个
```

所以 `LV_MEM_SIZE` 给 16KB 对 8.5KB 的峰值有约 1.9 倍余量。

**⚠ 但记住：`lv_malloc` 失败是不会报错的**，原因见 4.1。所以这个值不能给得太紧。

---

## 3. 诊断插桩：让看不见的东西看得见

这是**整个移植过程中最有价值的一个做法**，单独开一章。

### 3.1 为什么要插桩

"屏幕上什么都没有"这一个现象，可能的原因有一长串：

```
LVGL 没初始化 → 显示设备没建成 → 控件没建出来 → 事件没触发 flush
→ flush 送了但屏没显示 → 屏的初始化就不对
```

靠猜是分不开的。而**分级打点**一打出来，一眼就知道断在哪一步。

### 3.2 分级打点的样子

```c
[LVGL] start                    ← 准备初始化
[LVGL] init ok used=2080/...    ← 内核起来了
[LVGL] display create...
[LVGL] disp=0x20011070          ← 显示设备建出来了 (全 0 就是失败了)
[LVGL] display ok used=6068/...
[LVGL] indev ok used=6480/...
[LVGL] demo built used=8512/...  ← 控件建完了
[LVGL] flush#1 128x40 @0,0       ← 第一次真正往屏上送数据
[LVGL] running ... flush=6       ← 计数在涨 = 一直在送
```

### 3.3 ⚠ 两个必须注意的细节

**(a) 打点要限条数。** 串口 9600 下**一个字节约 1ms**。真让日志在循环里刷屏，
光打印就能把系统拖成"看起来卡死"——那反而掩盖了真正要查的问题。
所以回调里加了上限：

```c
#define LVGL_LOG_MAX   20U
static void Lvgl_LogCb(lv_log_level_t level, const char *buf)
{
    if (s_log_cnt >= LVGL_LOG_MAX) { return; }
    s_log_cnt++;
    ...
}
```

**(b) 诊断信息一律用 ASCII。** 中文经串口出去是 UTF-8，而很多串口助手按 GBK 解码，
到屏上就是乱码。这次就因为这个把"栈溢出! 任务 = "后面的**任务名整个丢了**，
白白多猜了一轮。ASCII 打点（`[LVGL]` / `[STACK]`）在任何助手上都可读。

### 3.4 诊断要挂在开关后面

照抄工程里已有的 `UI_DIAG` 套路：

```c
#define LVGL_DIAG 1     /* 排查完改成 0，相关代码就不会被编译进去 */
```

调用方**不用**在每处套 `#if` —— 在头文件里把关闭时的版本定义成空操作：

```c
#if (LVGL_DIAG != 0)
void Lvgl_DiagReport(const char *tag);
void Lvgl_DiagMsg(const char *msg);
#else
#define Lvgl_DiagReport(tag)   do { } while (0)
#define Lvgl_DiagMsg(msg)      do { } while (0)
#endif
```

这样插桩代码可以一直留在原地，只改一个开关。

### 3.5 顺手把 LVGL 自己的日志接到串口

LVGL 有一套日志系统，**打开它等于免费获得一个内线**。两个注意点：

```c
#define LV_USE_LOG    1
#define LV_LOG_PRINTF 0      /* ⚠ 千万别用 printf */
#define LV_LOG_LEVEL  LV_LOG_LEVEL_INFO
```

- **`LV_LOG_PRINTF` 必须是 0**：默认走 `printf` → newlib-nano 的 `_write` →
  裸机工程通常没实现（链接时那句 `_write is not implemented and will always fail`
  就是它）→ 日志石沉大海。正确做法是注册自己的回调：

```c
lv_log_register_print_cb(Lvgl_LogCb);   /* 内部转投到 Usart */
```

- **级别用 INFO 而不是默认的 WARN**：最关键的那条
  `"couldn't allocate memory (%lu bytes)"` **是 INFO 级**的。
  只开 WARN 的话，内存不足这种最要命的失败反而看不见。

---

## 4. 踩过的坑

### 4.1 ⚠⚠ LVGL 的断言默认全关 → 静默解引用 NULL

**这是这次最贵的一个坑，也是现象最有欺骗性的一个。**

`LV_USE_ASSERT_MALLOC` / `_NULL` / `_OBJ` / `_STYLE` 在 `lv_conf_internal.h` 里的
**默认值全是 0**。于是 `lv_theme_default_init()` 里这段代码是**裸的**：

```c
if(!lv_theme_default_is_inited()) {
    theme_def = lv_malloc_zeroed(sizeof(my_theme_t));
    LV_ASSERT_MALLOC(theme_def);        /* ← 空操作! */
}
my_theme_t * theme = theme_def;
...
if(theme->inited && ...)                /* ← 分配失败就在这里解引用 NULL */
```

**现象**：屏幕没有任何变化，LED 也不闪了，串口一片安静。

**为什么静默**：解引用 NULL → HardFault → 而 `HardFault_Handler` 长这样：

```c
void HardFault_Handler(void)
{
  while (1) { }      /* ST 库的默认实现, 什么都不说 */
}
```

整个系统就此死掉，**没有任何输出**。

**解法**：在 `lv_conf.h` 里覆盖 `LV_ASSERT_HANDLER`（`lv_assert.h` 里是 `#ifndef`
保护的，所以定义了就生效）：

```c
#ifndef __ASSEMBLY__
void Lvgl_AssertFail(const char *file, int line);
#define LV_ASSERT_HANDLER do { Lvgl_AssertFail(__FILE__, __LINE__); } while (0)
#endif
```

实现照抄工程里 `vApplicationStackOverflowHook` 的套路：打印 + 点亮 LED + 自旋。
**把"静默停住"变成"说清楚为什么停"**，是这次移植里单笔收益最大的一个改动。

⚠ 包 `#ifndef __ASSEMBLY__` 是因为 `lv_conf.h` 在汇编上下文也会被包含
（`lvgl_public.h` 的 `__ASSEMBLY__` 分支），而函数声明不是合法汇编。

### 4.2 ⚠⚠ 栈溢出 vs 内存不足：我误诊了一轮

**这个坑的价值在于"我把错误的方向修得很彻底"。**

**现象**：接上 LVGL 后屏幕没反应，LED 也不正常。

**我的判断（错的）**：内存不够。理由听起来很充分 —— 默认主题有 43 个 style，
`lv_theme_default_init` 里那个 malloc 失败会解引用 NULL（见 4.1），
而 24KB 的池子"看起来"不够。

**我做的**：把 `LV_MEM_SIZE` 从 24KB 一口气提到 **40KB**。

**结果**：**没用**。而且白占 24KB RAM —— 用户当场就问"RAM 怎么用了这么多"。

**真正的根因**：**UiTask 的栈溢出**。串口打出来的是：

```
[RTOS] 栈溢出! 任务 = ...
```

**为什么两个原因现象一模一样**：栈溢出 → `vApplicationStackOverflowHook` 打印 + 自旋；
HardFault → `HardFault_Handler` 的 `while(1)`。**两者都是"系统停住、没画面、LED 不动"。**
光看现象根本分不开。

**决定性的判别证据**：**从头到尾没有出现 `flush#1`**。
一次 flush 都没完成，说明死在**第一次 `lv_timer_handler()` 里面** —— 也就是 LVGL 的
绘制路径。而"内存不够"应该表现为分配失败 + HardFault，不会恰好死在第一次绘制上。

**真凶的量化**：栈给到 2048 字之后，实测 `[STACK] free=1165 words`
→ 峰值用了 `2048 - 1165 = 883` 字。而当时给的是 **768 字 < 883，必然溢出**。

**教训**：
1. **加内存之前先量。** 实测 `used=8512/39004`，池子只用了 21% —— 24KB 本来就够。
2. **"不够用"和"用不上"是两件事。** 我按最坏情况把池子开到最大，
   但最坏情况根本不存在。
3. **栈的深度不是常数。** LVGL 画一屏的调用链随绘制内容变化，
   这种量只能测，估算两次都偏差很大。

### 4.3 量栈和量堆的标准工具

这两个 API 记住就够了，别用估的：

```c
/* 栈: 当前任务"历史上最少还剩多少" —— 单位是**字(4 字节)**, 不是字节。
   ⚠ 是历史最低点, 所以这个数只会变小, 记录的是最坏情况。
   ⚠ 越小越危险, 经验上别低于总量的 1/4。 */
UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);

/* 堆: LVGL 自己的池子 */
lv_mem_monitor_t mon;
lv_mem_monitor(&mon);
/* mon.total_size / mon.free_size / mon.max_used / mon.frag_pct */
```

⚠ `uxTaskGetStackHighWaterMark` 需要 `FreeRTOSConfig.h` 里
`INCLUDE_uxTaskGetStackHighWaterMark = 1`。

**本工程的实测记录**（留作以后改动的对照基准）：

| 项 | 值 | 依据 |
|---|---|---|
| `LV_MEM_SIZE` | 16 KB | 峰值 9212 B，约 1.8 倍余量 |
| `UI_TASK_STACK` | 2048 字 (8KB) | 峰值 883 字，留 2.3 倍 |
| `configTOTAL_HEAP_SIZE` | 24 KB | 要装下 8KB 的栈，各任务合计约 11.6KB |
| LVGL 画布缓冲 | 128×40 (10KB) | 一屏分 4 次 flush；全屏要 40KB，太贵 |

### 4.4 串口编码：把自己的诊断搞丢了

UTF-8 的中文经串口出去，在按 GBK 解码的串口助手上是乱码。
`[RTOS] 栈溢出! 任务 = ` 后面的任务名就是这么丢的 —— 而那个任务名正好是
判断"哪个任务溢出了"的唯一线索。

**规则：诊断输出一律用 ASCII。** 实在要中文，先把串口助手的编码改成 UTF-8。

### 4.5 新版版本里的 API 变更坑

移植完能编译之后，编译器会告诉你大部分废弃项（如果开了 `-Wall`）。这次遇到两个：

- **`lv_obj_remove_flag()` 在 v9.6 已废弃**，要换成专用的
  `lv_obj_set_scrollable(obj, false)` 之类。废弃提示原文写得很清楚：
  `Use the dedicated lv_obj_set_<flag>() setters instead`
- **`LV_COLOR_DEPTH` 被 `LV_COLOR_FORMAT_DEFAULT` 取代**

还有一个**编译器不会告诉你**的语义变更：

- **`lv_group_set_default()` 只是把一个指针存起来，不会让之后新建的控件自动加入分组。**
  必须逐个 `lv_group_add_obj()`。网上有些教程说会自动加 —— **以源码为准**。

### 4.6 `file(GLOB)` 的工程规矩

`CMakeLists.txt` 用 `file(GLOB ...)` 收集源文件，**没有 `CONFIGURE_DEPENDS`** ——
CMake 只在 configure 阶段展开通配符。所以：

> **新增 / 删除 / 改名源文件之后，必须重跑 `cmake -S . -B build -G Ninja`。**

只跑 `cmake --build build` 的话 Ninja 根本不知道有新文件，会报
`undefined reference to xxx` —— 而你会以为是代码写错了，然后去找一个不存在的拼写错误。

### 4.7 编辑器报 `#include` 错、但 cmake 编得过

**那多半是 VSCode 的 C/C++ 扩展在用一份过时的配置，和实际编译无关。**

判断"到底是不是真错"的黄金标准永远是 **`cmake --build build` 过不过**。

解法是在 `CMakeLists.txt` 里设 `CMAKE_EXPORT_COMPILE_COMMANDS ON`，
生成 `build/compile_commands.json` —— 扩展直接读真实的编译命令，改目录结构再也不用管它。

---

## 5. 出问题时先看哪里

把这次用过的决策树固化下来：

| 现象 | 先查 | 怎么查 |
|---|---|---|
| 屏幕完全没反应 | LED 还闪吗 | 不闪 → 系统死了（HardFault/断言）；正常闪 → 系统活着，是静默失败 |
| LED 快闪 | UiTask 卡住了 | `s_ui_alive` 计数没涨。看是不是某个循环里漏了自增 |
| 串口一条 LVGL 日志都没有 | 日志接了吗 | `LV_USE_LOG` + `lv_log_register_print_cb`，且 `LV_LOG_LEVEL` 要 INFO |
| 屏上什么都没有，但串口说 flush 在跑 | 送的**内容**是什么 | 把缓冲里的像素值打出来（`min`/`max`/`diff`）；`diff==0` = 渲染就是空的 |
| 断言/HardFault 静默 | 断言处理器覆盖了吗 | `LV_ASSERT_HANDLER` 覆盖成"打印后停" |
| 系统停住但不知道停在哪 | 分级打点 | 在每一步之间插一行 `[XXX] 到这一步了` |
| 内存到底用了多少 | 不要估 | `lv_mem_monitor().max_used` |
| 栈到底用了多少 | 不要估 | `uxTaskGetStackHighWaterMark(NULL)` |
| `undefined reference` | 重跑 cmake 了吗 | `cmake -S . -B build -G Ninja` |
| 颜色不对（红蓝互换等） | 字节序 | 见 2.4；**注意这不会是"没画面"的原因** |

**一条通用原则**：

> **现象只有一个，但可能的原因有一串时，不要猜 —— 插桩。**
> 一行打点的成本是几秒钟，一次误诊的成本是好几个烧录来回加 24KB 内存。

---

## 6. 附：关键配置速查

### `FreeRTOSConfig.h`

```c
#define configCPU_CLOCK_HZ                      (168000000UL)
#define configTICK_RATE_HZ                      1000
#define configTOTAL_HEAP_SIZE                   (24 * 1024)
#define configMINIMAL_STACK_SIZE                128        /* 单位: 字 */
#define configUSE_TICK_HOOK                     1
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_TIMERS                        0          /* 用不到就关, 省一个任务 */
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5
#define INCLUDE_uxTaskGetStackHighWaterMark     1          /* 量栈用 */
#define configASSERT(x)  if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) { } }

/* 中断向量改名 */
#define vPortSVCHandler       SVC_Handler
#define xPortPendSVHandler    PendSV_Handler
#define xPortSysTickHandler   SysTick_Handler
```

⚠ **配套必须在 `main()` 最开头**：`NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4)`

### `lv_conf.h`（本工程实际改过的项）

```c
#define LV_MEM_SIZE                 (16U * 1024U)   /* 实测峰值 9212 B */
#define LV_USE_OS                   LV_OS_NONE      /* 见 2.5 */
#define LV_COLOR_FORMAT_DEFAULT     LV_COLOR_FORMAT_RGB565
#define LV_ATTRIBUTE_MEM_ALIGN      __attribute__((aligned(4)))
#define LV_USE_LOG                  1               /* 接到串口, 见 3.5 */
#define LV_LOG_LEVEL                LV_LOG_LEVEL_INFO
#define LV_LOG_PRINTF               0               /* ⚠ 不能用 printf */
#define LV_USE_THEME_SIMPLE         0               /* 用不到 */
#define LV_USE_THEME_MONO           0
/* 断言处理器覆盖, 见 4.1 —— 这个一定要加 */
#ifndef __ASSEMBLY__
void Lvgl_AssertFail(const char *file, int line);
#define LV_ASSERT_HANDLER do { Lvgl_AssertFail(__FILE__, __LINE__); } while (0)
#endif
```

### 三个数字的确定顺序（推荐流程）

1. **先给宽**：`LV_MEM_SIZE` / `UI_TASK_STACK` 都给个明显偏大的值，先让它跑起来
2. **量**：跑起来后看 `lv_mem_monitor().max_used` 和 `uxTaskGetStackHighWaterMark()`
3. **再收**：按实测值给 **2 倍左右**余量，别按实测值卡死

⚠ 顺序不能反。先抠大小再跑，就是在拿"系统静默死掉"赌一个你还没量过的数。
