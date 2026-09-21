# SoftWare/ —— 代码分层规则

2026-09-22 重组。原来这里是平铺的 `inc/` + `src/`，`main.c` 涨到 437 行，
状态机、串口、LED、主循环全挤在一起。现在按**功能领域**分目录，
`main.c` 只剩一张初始化清单（61 行）。

## 目录

每个领域都是 `inc/` + `src/` 两层，共 6 个领域：

| 领域 | 管什么 | 模块 |
|---|---|---|
| `system/` | 和芯片/板子最贴近的基础件 | `Tick`(1ms 时基) `Led`(心跳灯) `Ring_buffer`(纯软件环形缓冲) |
| `comms/` | 对外通信 | `Usart`(调试串口，含接收中断) |
| `display/` | 屏幕上的一切 | `LCD`(ST7735S 驱动) `Menu`(三个界面的绘制) `Font8x16` `CN_Font` |
| `camera/` | 摄像头 | `OV7670`(带 AL422B FIFO 的模块) |
| `motion/` | 会动的东西 | `Servo`(SG90) `Encoder`(旋转编码器) |
| `app/` | 应用逻辑 + 胶水 | `App`(界面状态机、串口回显、开机横幅) |

`USER/main.c` 在 `SoftWare/` 外面 —— 它只是入口：一份初始化清单 + 一个
两行的主循环，**看一眼就知道这块板子跑了些什么**。

## 依赖规则

1. **只能往下依赖，不能往上。** 上层可以用下层，反过来不行。
2. **同层之间不许互相依赖。** `camera/` 不认识 `display/`，`motion/` 不认识 `comms/`。
3. **`system/` 是纯底层**，不依赖任何领域，谁都能用，也能整块搬到别的工程。
4. **跨领域的胶水放 `app/`。** 一件事如果需要同时用到两个领域（比如"读摄像头
   寄存器然后从串口打出来"），就写在 `App.c` 里 —— 不要让那两个领域互相 include。

### 当前实际依赖（改完代码记得回来对一眼）

```
system/   ->  （无）
comms/    ->  system
display/  ->  （无）
camera/   ->  system
motion/   ->  system
app/      ->  camera  comms  display  motion  system
```

自查命令：

```bash
for d in system comms display camera motion app; do
  deps=$(grep -rho '#include "[A-Za-z0-9_]*\.h"' SoftWare/$d/ | sed 's/#include "//;s/"//' | sort -u)
  out=""
  for h in $deps; do for d2 in system comms display camera motion app; do
      if [ -f "SoftWare/$d2/inc/$h" ] && [ "$d2" != "$d" ]; then out="$out $d2"; fi
  done; done
  printf "%-9s -> %s\n" "$d/" "$(echo $out | tr ' ' '\n' | sort -u | tr '\n' ' ')"
done
```

**已经踩过一次**：最开始 `Tick.c` 里 `#include "Encoder.h"`（SysTick 中断要
驱动按键采样），于是 `system/ -> motion`，底层依赖了上层。改成 Tick 提供
`Tick_SetMsCallback()` 回调注册口、由 `Encoder_Init()` 主动注册自己之后，
方向才正过来。**要回调就用这种反转，别直接 include。**

## 新文件放哪

问自己两个问题：

1. **它碰寄存器/引脚吗？** 碰 → 对应领域（摄像头相关进 `camera/`，
   电机/编码器进 `motion/`，屏幕进 `display/`，串口进 `comms/`）
2. **它是纯数据或纯算法吗？** 是 → 看被谁用。字模跟着 `display/`，
   环形缓冲跟着 `system/`

都不太像 → 大概率是 `app/` 的胶水逻辑。

## 加一个新外设的完整流程

1. 新模块放进对应领域（或在 `APP_DOMAINS` 里加一个新领域）
2. 在 `USER/main.c` 的初始化清单里加一行
3. 需要界面的话，在 `display/Menu.c` 的 `s_items[]` 和 `app/App.c` 的状态机里各加一项
4. **重跑 cmake**（见下）

## ⚠ 编译注意

`CMakeLists.txt` 用 `file(GLOB ...)` 收集源文件，**没有 `CONFIGURE_DEPENDS`** ——
CMake 只在 configure 阶段展开通配符。所以：

- **新增/删除/改名了源文件之后，必须重跑** `cmake -S . -B build -G Ninja`
- 只跑 `cmake --build build` 的话 Ninja 根本不知道有新文件，
  会报 `undefined reference to xxx`，而你会以为是代码写错了

头文件搜索路径由 `CMakeLists.txt` 的 `APP_DOMAINS` 变量统一生成，
加新领域时只要往那个列表里加个名字，include 路径和源文件收集都会跟上。

### 编辑器报 #include 错、但 cmake 编得过？

**那多半是 VSCode 的 C/C++ 扩展在用一份过时的配置，和实际编译无关。**

2026-09-22 重组目录时就撞过一次：`.vscode/c_cpp_properties.json` 里写死了
`SoftWare/inc`，目录一没，所有自己写的模块都报
"基于 configurationProvider 检测到 #include 错误"，而 `cmake --build` 一直是好的。

现在 `CMakeLists.txt` 里设了 `CMAKE_EXPORT_COMPILE_COMMANDS ON`，会生成
`build/compile_commands.json` —— 扩展直接读真实的编译命令，**改目录结构
再也不用管它**。配置里那份 `includePath` 只是兜底。

还报错的话按顺序试：

1. `cmake -S . -B build -G Ninja`（重新生成 `compile_commands.json`）
2. VSCode 命令面板 → **`C/C++: Reset IntelliSense Database`**
3. 还不行就 **`Developer: Reload Window`**

⚠ 判断"到底是不是真错"的黄金标准永远是 **`cmake --build build` 过不过**。

## 中断处理函数放哪

**跟着它的驱动模块走**，不要集中到 `System/stm32f4xx_it.c`：

| 中断 | 位置 |
|---|---|
| `EXTI9_5` / `TIM7` | `motion/src/Encoder.c` |
| `SysTick` | `system/src/Tick.c` |
| `USART1` | `comms/src/Usart.c` |
| 内核异常（HardFault 等） | `System/stm32f4xx_it.c`（只剩这些） |

好处是**中断和它读写的变量在同一个文件里**，想查"谁改了这个变量"不用跳文件。
拆之前 `USART1_IRQHandler` 在 `it.c`、环形缓冲在 `main.c`，来回跳很容易看漏。

⚠ 一个中断向量只能定义一次 —— 在 `it.c` 里重新定义会链接报
`multiple definition of ..._IRQHandler`。
