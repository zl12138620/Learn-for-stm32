# ==============================================================================
# CMakeLists.txt —— STM32F407VGT6 (Cortex-M4F) 裸机工程
# 构建系统   : CMake + Ninja + Arm GNU GCC (arm-none-eabi) + OpenOCD/pyOCD
# 外设库     : STM32F4xx_DSP_StdPeriph_Lib_V1.9.0 (标准外设库)
# 目标产物   : build/output/STM32F407VGT6.elf/.hex/.bin/.map
#
# 使用方法:
#   cmake -S . -B build -G Ninja          # 配置(Windows 显式指定 Ninja)
#   cmake --build build                    # 编译并链接
#   产物输出到 build/output/
# ==============================================================================

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0123 NEW)

# ---------- 1/2. 目标平台：裸机 Cortex-M4F (无操作系统) ----------
set(CMAKE_SYSTEM_NAME    Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m4)

# ---------- 3. 工具链文件 ----------
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake")
endif()

# ---------- 4. 工程 (C + ASM) ----------
project(STM32F407VGT6 C ASM)

# 默认 Debug，可用 -DCMAKE_BUILD_TYPE=Release 切换
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type: Debug or Release" FORCE)
endif()

# ---------- 4b. 给编辑器导出 compile_commands.json ----------
# 少了它, VSCode 的 C/C++ 扩展只能靠 .vscode/c_cpp_properties.json 里那份
# **手写的** includePath 去解析 #include —— 那个东西不会跟着 CMake 变,
# 目录一搬家(比如 2026-09-22 按领域重组 SoftWare/)全部头文件就报
# "基于 configurationProvider 检测到 #include 错误"。
# 导出之后扩展直接读真实的编译命令, 以后改目录结构再也不用管它。
#
# 只在不是 MSVC 的场合设(传统上是给 clangd/C-C++ 扩展用的习惯写法)。
if(NOT MSVC)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(TARGET_NAME STM32F407VGT6)
set(OUTPUT_DIR  ${CMAKE_BINARY_DIR}/output)
set(LIB_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/STM32F4xx_DSP_StdPeriph_Lib_V1.9.0)

string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)

# ---------- 5. 构建类型对应的优化选项 ----------
if(BUILD_TYPE_UPPER STREQUAL "RELEASE")
  set(OPT_FLAGS -Os)
else()
  set(OPT_FLAGS -Og -g3)
endif()

# ---------- 6. 器件宏定义 ----------
# STM32F40_41xxx : STM32F407VGT6 所属的器件密度/系列
# USE_STDPERIPH_DRIVER : 启用标准外设库 (System/stm32f4xx.h 会 #include stm32f4xx_conf.h)
add_compile_definitions(STM32F40_41xxx USE_STDPERIPH_DRIVER)

# ---------- 7. 头文件搜索路径 ----------
#  USER : 入口 (main.c/main.h)
#  SoftWare/<领域>/inc : 自己写的库, 按功能领域分目录 (2026-09-22 重组)
#  依赖规则见 SoftWare/README.md —— 同层之间不互相依赖, 上层可依赖下层
#  System/CMSIS/Driver : ST 库头 (作为 SYSTEM include，抑制库头内部固有宏重定义噪声)
#
#  ui 放在 display 之后、app 之前: 它用 LCD/Tick, 自己又被 app 用。
set(APP_DOMAINS system comms display camera motion ui app)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/USER)
foreach(domain ${APP_DOMAINS})
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SoftWare/${domain}/inc)
endforeach()

# FreeRTOS 内核头(第三方)。放 SYSTEM 里, 免得它内部的宏重定义污染我们的编译输出。
# ⚠ FreeRTOSConfig.h 不在这里 —— 那是**本工程的配置**, 在 SoftWare/system/inc/,
#   走上面的领域路径。内核源码一个字都不改, 要调行为只改那个配置。
include_directories(SYSTEM
  ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/include
  ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/portable/GCC/ARM_CM4F
)

# LVGL 内核头(第三方, v9.6.0), 同样放 SYSTEM 里。
#
# ⚠ 只需要**根目录**这一个路径。v9.6 的头文件布局是这样:
#     LVGL/lvgl.h          -> #include "include/lvgl/lvgl.h"
#     LVGL/include/lvgl/**    内部彼此全是相对包含("core/lv_obj.h")
#     LVGL/src/**.c           包含 "../include/lvgl/lvgl.h" 也是相对路径
#   也就是说只要进得了 LVGL/ 的根, 剩下的 LVGL 自己全用相对路径走通了。
#   本工程代码因此直接写 #include "lvgl.h" 就行。
include_directories(SYSTEM
  ${CMAKE_CURRENT_SOURCE_DIR}/LVGL
)

include_directories(SYSTEM
  ${CMAKE_CURRENT_SOURCE_DIR}/System
  ${LIB_ROOT}/Libraries/CMSIS/Include
  ${LIB_ROOT}/Libraries/STM32F4xx_StdPeriph_Driver/inc
)

# ---------- 8. 源文件收集 ----------
file(GLOB USER_SOURCES   ${CMAKE_CURRENT_SOURCE_DIR}/USER/*.c)
file(GLOB SYSTEM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/System/*.c)
file(GLOB SPL_SOURCES    ${LIB_ROOT}/Libraries/STM32F4xx_StdPeriph_Driver/src/*.c)

# SoftWare/ 下每个领域一个 src/ 目录, 挨个收。
# 加新领域时只要往上面 APP_DOMAINS 里加个名字, 这里和 include 路径都会跟上。
#
# ⚠ 这里用的是不带 CONFIGURE_DEPENDS 的 file(GLOB): CMake 只在 configure
#   阶段展开通配符。所以**新增/删除/改名了源文件之后, 必须重跑**
#     cmake -S . -B build -G Ninja
#   只 cmake --build 的话 Ninja 根本不知道有新文件, 会报 undefined reference。
set(APP_SOURCES "")
foreach(domain ${APP_DOMAINS})
  file(GLOB domain_sources ${CMAKE_CURRENT_SOURCE_DIR}/SoftWare/${domain}/src/*.c)
  list(APPEND APP_SOURCES ${domain_sources})
endforeach()

# FreeRTOS 内核源码(第三方, 见 FreeRTOS-Kernel/README.md)。
# 根目录那几个 .c 全是内核本体, 整体收没问题;
# ⚠ 但 **portable/ 绝对不能整体 GLOB** —— 它下面每个架构/编译器都有同名
#   port.c(还有各种 heap_x.c), 一起编译会重复定义且大量编译不过。
#   只显式挑本工程这条路径: GCC + Cortex-M4F(带 FPU) + heap_4。
file(GLOB FREERTOS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/*.c)
list(APPEND FREERTOS_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/portable/GCC/ARM_CM4F/port.c
  ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/portable/MemMang/heap_4.c
)

# LVGL 内核源码(第三方, v9.6.0), 483 个 .c。
#
# ⚠ 这里敢用 GLOB_RECURSE 一次全收, 是因为两件事:
#     1. 关掉的控件/驱动**不需要手工排除** —— LVGL 每个 .c 内部自己就有
#        #if LV_USE_XXX 守卫, 没开的编出来是个空文件(比如 lv_button.c
#        整个被 #if LV_USE_BUTTON 包着)。
#     2. 真正没用到的代码由链接期的 --gc-sections 丢掉(见第 12 节的链接选项)。
#   所以"收多了"的代价只是**编译时间**, 不是 flash 占用。
#
# ⚠ 只收 *.c。src/draw/sw/blend/helium/ 下那个 lv_blend_helium.S 是
#   Helium 汇编, 归 LV_USE_NATIVE_HELIUM_ASM 管(关着的), 且不在通配范围内。
file(GLOB_RECURSE LVGL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/LVGL/src/*.c)

# LVGL 的配置入口。lv_conf_internal.h 见到这个宏就 #include "lv_conf.h",
# 并且是**走 include 路径**去找 —— 我们的配置放在 SoftWare/ui/inc/(已经
# 因为 APP_DOMAINS 里的 ui 进了搜索路径), 而不是官方默认的
# "lv_conf.h 摆在 lvgl 文件夹旁边"。详见 SoftWare/ui/inc/lv_conf.h 开头。
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

# lv_font_conv 生成的字体文件里带这么一段:
#     #ifdef LV_LVGL_H_INCLUDE_SIMPLE
#     #include "lvgl.h"
#     #else
#     #include "lvgl/lvgl.h"      <-- 需要 LVGL/include 也在搜索路径上
#     #endif
# 不定义这个宏的话会走 else 分支去找 "lvgl/lvgl.h", 而那个路径我们**故意**没加
# (见上面 include_directories 的注释: 进得了 LVGL/ 根目录, 剩下的 LVGL 自己
#  全用相对路径走通了, 所以只需要一个路径)。
# 定义它让生成的文件走 #include "lvgl.h", 复用同一个路径, 不用再加第二个。
add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)

# STM32F407 (STM32F40_41xxx) 没有以下外设模块，排除以免编译报错
# (DMA2D/LTDC/DSI/SAI/QSPI/SPDIFRX/FMC/FMPI2C/LPTIM/CEC/DFSDM)
list(FILTER SPL_SOURCES EXCLUDE REGEX
  "/(stm32f4xx_dma2d|stm32f4xx_ltdc|stm32f4xx_dsi|stm32f4xx_sai|stm32f4xx_qspi|stm32f4xx_spdifrx|stm32f4xx_fmc|stm32f4xx_fmpi2c|stm32f4xx_lptim|stm32f4xx_cec|stm32f4xx_dfsdm)\\.c$")

# GCC 语法启动文件 (由 MDK 版 startup_stm32f40_41xxx.s 对照生成;
# .S 大写扩展名使 GNU as 先运行 C 预处理器)
set(STARTUP_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/Startup/startup_stm32f40_41xxx_gcc.S)

# ---------- 9/10. 链接脚本 ----------
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/stm32f407vgt6_flash.ld)

# ---------- 11. 编译选项 (C 与 ASM 共用 CPU/FPU 选项) ----------
add_compile_options(-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16)

# ---------- 12/13. 目标与链接 ----------
add_executable(${TARGET_NAME}.elf
  ${STARTUP_SOURCE}
  ${USER_SOURCES}
  ${SYSTEM_SOURCES}
  ${APP_SOURCES}
  ${FREERTOS_SOURCES}
  ${LVGL_SOURCES}
  ${SPL_SOURCES}
)

# C 专属编译选项 (ASM 不需要 -std/-Wall/优化节)
target_compile_options(${TARGET_NAME}.elf PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-Wall>
  $<$<COMPILE_LANGUAGE:C>:-ffunction-sections;-fdata-sections>
  $<$<COMPILE_LANGUAGE:C>:${OPT_FLAGS}>
)

# 链接选项：-T 链接脚本 + --gc-sections + Map + 内存占用打印
target_link_options(${TARGET_NAME}.elf PRIVATE
  -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
  -T ${LINKER_SCRIPT}
  --specs=nano.specs          # newlib-nano (精简 C 库)
  --specs=nosys.specs         # nosys (无宿主系统桩函数)
  -Wl,--gc-sections
  -Wl,-Map=${OUTPUT_DIR}/${TARGET_NAME}.map
  -Wl,--print-memory-usage
)

# ---------- 14. 输出位置与文件名 ----------
set_target_properties(${TARGET_NAME}.elf PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}
  OUTPUT_NAME              ${TARGET_NAME}.elf
)

# ---------- POST_BUILD: 生成 .hex / .bin / size 报告 ----------
add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET_NAME}.elf> ${OUTPUT_DIR}/${TARGET_NAME}.hex
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET_NAME}.elf> ${OUTPUT_DIR}/${TARGET_NAME}.bin
  COMMAND ${CMAKE_SIZE}              $<TARGET_FILE:${TARGET_NAME}.elf>
  COMMENT "Generate ${TARGET_NAME}.hex/.bin and print memory usage"
)
