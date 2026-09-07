# =============================================================================
# STM32F407VGT6 —— CMake + GCC + OpenOCD 构建与调试指南
# Build & Debug Guide (中文 / English)
# =============================================================================

## 1. 工程概述 / Overview
裸机工程，主控 **STM32F407VGT6**（Cortex-M4F，1MB Flash / 192KB RAM，
另有 64KB CCMRAM），外设库使用 **STM32F4xx_DSP_StdPeriph_Lib_V1.9.0**
（标准外设库 StdPeriph），构建工具为 **CMake + Ninja + Arm GNU GCC**
(arm-none-eabi)，下载/调试可用 **OpenOCD**（ST-Link）或 **pyOCD**（CMSIS-DAP）。

已添加的 CMake 构建方式与原有目录互不干扰（未改动任何 Keil 工程文件）。

### 目录结构 / Layout
```
STM32F407VGT6/
├── CMakeLists.txt                     # 主构建脚本
├── cmake/toolchain-arm-none-eabi.cmake
├── stm32f407vgt6_flash.ld             # GNU ld 链接脚本
├── USER/                              # 应用层: main.c / main.h
├── System/                            # CMSIS 设备层: stm32f4xx.h(器件头)、
│                                      #   stm32f4xx_conf.h、stm32f4xx_it.c/h、
│                                      #   system_stm32f4xx.c/h
├── Startup/                           # 启动文件
│   ├── startup_stm32f40_41xxx_gcc.S   # [新] GCC 版启动文件(供本 CMake 构建)
│   └── startup_stm32f40_41xxx.s       # Keil/MDK 版(原工程使用, 保留不动)
├── SoftWare/                          # 自建软件组件(当前 SoftUSART 为占位空文件)
├── pyocd/                            # STM32F4 器件包(供 pyocd 使用, 见 PYOCD-器件包下载与使用.md)
├── STM32F4xx_DSP_StdPeriph_Lib_V1.9.0 # ST 标准外设库(CMSIS/Driver)
├── build/                             # CMake 输出(Debug 默认)
├── build-release/                     # CMake 输出(Release 示例, 可自行删除)
└── .vscode/                           # tasks/launch/c_cpp_properties
```

## 2. 前提工具 / Prerequisites
| 工具 | 版本要求 | 检查命令 |
|---|---|---|
| CMake   | ≥ 3.20 | `cmake --version` |
| Ninja   | 任意   | `ninja --version` |
| arm-none-eabi-gcc | 建议 ≥ 10 | `arm-none-eabi-gcc --version` |
| OpenOCD(可选) | ≥ 0.11 | `openocd --version` |
| pyOCD(可选)   | ≥ 0.30 | `pyocd --version` |

## 3. 构建 / Build
```powershell
# Debug（默认, -Og -g3）
cmake -S . -B build -G Ninja
cmake --build build

# Release（-Os）
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```
产物位于 `build/output/`（或 `build-release/output/`）：
`STM32F407VGT6.elf / .hex / .bin / .map`。

## 4. 下载烧录 / Flash
> 本工程调试器为 CMSIS-DAP 类型（如野火 FireDAP），以下均按其接口编写；
> 若使用 ST-Link，把 `interface/cmsis-dap.cfg` 换成 `interface/stlink.cfg` 即可。
```powershell
# 方式一：CMSIS-DAP + OpenOCD
openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg `
        -c "program build/output/STM32F407VGT6.hex verify reset exit"

# 方式二：CMSIS-DAP + pyOCD
# 本工程已内置本地器件包（下载/使用见 PYOCD-器件包下载与使用.md）
pyocd flash -t stm32f407vg --pack pyocd\Keil.STM32F4xx_DFP.3.1.1.pack `
           build\output\STM32F407VGT6.hex --reset
```

## 5. VS Code 调试 / Debug
安装扩展 **Cortex-Debug**，然后：
1. 终端任务 **Terminal → Run Task → build** 生成固件；
2. 按 **F5** 并选择：
   - **Cortex-Debug (OpenOCD / CMSIS-DAP 野火 FireDAP)** —— 推荐，当前硬件即此配置；
   - Cortex-Debug (pyOCD / CMSIS-DAP) —— 器件包已装入 pyocd（`pyocd pack install stm32f407vg`），选它即用。
   会自动先编译、烧录并停在 `main`。

### 5.1 你的调试器该配哪个 interface cfg？（重要）
Cortex-Debug 的 OpenOCD 配置里 `interface/*.cfg` **必须和你手上的调试器匹配**，
否则会出现连接失败或找不到设备。

| 调试器 | launch.json 的 configFiles 用 | 备注 |
|---|---|---|
| 野火 FireDAP / DAPLink / 其它 CMSIS-DAP | `interface/cmsis-dap.cfg` | **本工程默认（推荐）** |
| ST-Link（板载或独立） | `interface/stlink.cfg` | 与 cmsis-dap 二选一 |
| J-Link | `interface/jlink.cfg` | 需 J-Link 软件 |

如何确认自己的调试器类型：
```powershell
pyocd list          # 输出里含 "CMSIS-DAP" 字样 = CMSIS-DAP 类调试器
```

相关文件：`.vscode/tasks.json`、`.vscode/launch.json`、
`.vscode/c_cpp_properties.json`（含 `compile_commands.json` 支持，IntelliSense 可用）。

## 6. 器件与库配置 / Device & library notes
- 器件宏：`STM32F40_41xxx`、`USE_STDPERIPH_DRIVER`（在 CMakeLists 中定义，
  对应 `System/stm32f4xx.h` 的分支选择）。
- 编译/链接：`-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16`
  （启用硬件浮点）。
- 时钟：天空星 STM32F407VGT6 板载 HSE 晶振为 **8MHz**，已在
  `System/stm32f4xx.h`（`HSE_VALUE = 8MHz`）与
  `System/system_stm32f4xx.c`（`PLL_M = 8`）中改好，SYSCLK 仍配置到 168MHz。
  若换用其它晶振频率的板子，请同步修改这两处（与 Keil 工程相同规则）。
- StdPeriph 驱动为全量收集再剔除 F407 不支持的模块（DMA2D/LTDC/DSI/SAI/
  QSPI/SPDIFRX/FMC/FMPI2C/LPTIM/CEC/DFSDM），见 CMakeLists 中的 FILTER。

## 7. 常见问题 / Troubleshooting
| 现象 | 处理 |
|---|---|
| Keil 能编但 GCC 编不过 | 启动文件必须是 GCC 语法（本工程用 `Startup/*_gcc.S`）；MDK 版 `.s`（`AREA/DCD`）不能交给 arm-none-eabi-gcc |
| 新增 .c/.h 未参与编译 | 放在已有 GLOB 目录（USER/System/SoftWare/src）后**重新运行 cmake 配置**；新目录则手动加入 CMakeLists |
| `_close/_read/_write is not implemented` 链接提示 | nosys 桩函数提示，裸机无宿主系统属正常，不影响 |
| `DBGMCU_APB2_* redefined` 警告 | ST 全系合一头文件固有噪声，功能无影响 |
| 中断不触发/进 HardFault | 检查中断是否已映射：向量表弱符号会被 `stm32f4xx_it.c` 中的同名强符号自动覆盖 |
| IntelliSense 找不到头 | 依据 `.vscode/c_cpp_properties.json`，并可用 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 后指向 `compile_commands.json` |
| `File not found "executable": …/build/output/STM32F407VGT6.elf` | 说明没生成 elf。先 `cmake -S . -B build -G Ninja` 再 `cmake --build build`，检查是否有链接错误（如 undefined reference，见第 9.4 节）；确认文件存在：`Test-Path build/output/STM32F407VGT6.elf` |
| `PyOCD: GDB Server Quit Unexpectedly` 或 `Failed to launch PyOCD GDB Server: Timeout.` | pyocd 本地缺 STM32F4 器件包，找不到 target。解决：`pyocd pack install stm32f407vg`（本工程已装），验证 `pyocd list --targets` 有 `stm32f407`；或改用 OpenOCD 配置（详见第 9.2 节、PYOCD-器件包下载与使用.md） |
| `At least one OpenOCD Configuration File must be specified` | launch.json 的 OpenOCD 配置必须用 **`configFiles`** 数组写 cfg，不能只在 `serverArgs` 里传 `-f`（详见第 9.3 节） |
| 换电脑/换调试器后连不上 | 先跑硬件链路自检命令，确认 interface cfg 与调试器类型匹配（详见第 9.5 节） |

## 8. 参考命令速查 / Cheat sheet
```powershell
cmake --build build                 # 编译
cmake --build build --target clean  # 清理
arm-none-eabi-size build/output/STM32F407VGT6.elf   # 查看 FLASH/RAM 占用
arm-none-eabi-objdump -h build/output/STM32F407VGT6.elf  # 段布局

# 硬件链路自检（换电脑/换调试器后先跑这个）
openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg `
        -c "adapter speed 1000" -c "init" -c "exit"
pyocd list                          # 查看已连接的调试器类型
pyocd list --targets                # 查看 pyocd 已有哪些器件 target
pyocd pack install stm32f407vg      # (pyocd 方案) 安装 STM32F4 器件包
```

## 9. 调试实录：VS Code 无法 F5 的完整排错（2026-09 实战）
> 以下 4 个报错都曾在本工程实际出现并解决。将来再遇到可按小节处理。

### 9.1 `File not found "executable": …/build/output/STM32F407VGT6.elf`
**原因链**：`build/` 目录不存在或链接失败 → 没有生成 `.elf`。
**处理**：
```powershell
cmake -S . -B build -G Ninja     # 若 build/ 缺失，先配置（tasks.json 的 build 已自动先做这步）
cmake --build build              # 观察是否出现链接错误
Test-Path build/output/STM32F407VGT6.elf   # 应返回 True
```
若链接报 `undefined reference to ...`，参见 9.4。

### 9.2 `PyOCD: GDB Server Quit Unexpectedly` / `Failed to launch PyOCD GDB Server: Timeout.`
**原因**：pyocd 本地没有该器件的 target（缺器件包），`targetId` 无法解析。
（两个报错同源：新版 pyocd gdbserver 找不到目标时直接退出，扩展即报上述其一。）
**验证**：
```powershell
pyocd list                          # 有 CMSIS-DAP probe 说明硬件 OK
pyocd list --targets | findstr /i 407   # 无输出 = 缺器件包
```
**解决**（本工程已执行）：
- `pyocd pack install stm32f407vg` 安装器件包，然后 `pyocd list --targets | findstr /i 407v` 确认；
- 或改用 **OpenOCD / CMSIS-DAP** 配置（本工程 launch.json 第一个）。
> 注：
> - Cortex-Debug 1.12.1（VS Code 商店稳定版）与 pyocd 0.36+ 存在兼容 bug：
>   即使器件包装好、GDB 也能连上，仍可能报 `Failed to launch PyOCD GDB Server: Timeout.`
>   （官方 issue #1222，已在预发布版修复）。
> - 解决办法：把 Cortex-Debug 升级到 **≥ 1.13.0-pre10**。本工程已把该 VSIX 下载到
>   `tools/cortex-debug-1.13.0-pre10.vsix`，安装命令：
>   ```powershell
>   code --install-extension tools\cortex-debug-1.13.0-pre10.vsix --force
>   # 然后 VS Code: Ctrl+Shift+P → Developer: Reload Window
>   ```
> - Cortex-Debug **不支持 `cmsisPack` 字段**（1.12/1.13 均无此配置项）；必须
>   `pyocd pack install`（详见 PYOCD-器件包下载与使用.md）。

### 9.3 `At least one OpenOCD Configuration File must be specified`
**原因**：Cortex-Debug 只认 `configFiles` 字段判断"是否有配置文件"；
不能只写 `serverArgs: ["-f", ...]`。
**正确写法**（本工程 launch.json 已经是）：
```jsonc
"servertype": "openocd",
"configFiles": ["interface/cmsis-dap.cfg", "target/stm32f4x.cfg"], // 必须用这个字段
"interface": "swd",
"serverArgs": ["-c", "adapter speed 1000"],                        // 可选的附加参数
"executable": "${workspaceFolder}/build/output/STM32F407VGT6.elf",
"preLaunchTask": "build",
"runToEntryPoint": "main"
```

### 9.4 `undefined reference to TimingDelay_Decrement`（链接失败）
**场景**：把 USER/main.c 换成自己的代码后链接报这个错。
**原因**：`System/stm32f4xx_it.c` 中模板 `SysTick_Handler` 调用模板软延时
`TimingDelay_Decrement()`，而新 main 不再提供该函数。
**处理**（本工程已做，以后遇到照做即可）：
把 `System/stm32f4xx_it.c` 的 `SysTick_Handler` 内调用注释掉：
```c
void SysTick_Handler(void)
{
  /* TimingDelay_Decrement(); */
}
```
若以后想用模板的 `Delay()` 软延时：恢复该调用，并在 main 中提供
`TimingDelay_Decrement()` 定义（参考 ST 模板 main.c 的 Delay 实现）。
`USER/main.h` 保留声明不影响链接（无人调用即无引用）。

### 9.5 硬件链路自检（换电脑 / 换板 / 换调试器后先跑）
```powershell
openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg `
        -c "adapter speed 1000" -c "init" -c "exit"
```
看到 `Cortex-M4 ... processor detected` 即链路 OK。
注意：该命令以"非 0 退出码"结束属正常（OpenOCD 日志输出到 stderr，
PowerShell 会因此把退出码记为 1），以日志内容为准。
