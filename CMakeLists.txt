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
#  USER/SoftWare : 应用代码 (保留自身告警)
#  System/CMSIS/Driver : ST 库头 (作为 SYSTEM include，抑制库头内部固有宏重定义噪声)
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/USER
  ${CMAKE_CURRENT_SOURCE_DIR}/SoftWare/inc
)
include_directories(SYSTEM
  ${CMAKE_CURRENT_SOURCE_DIR}/System
  ${LIB_ROOT}/Libraries/CMSIS/Include
  ${LIB_ROOT}/Libraries/STM32F4xx_StdPeriph_Driver/inc
)

# ---------- 8. 源文件收集 ----------
file(GLOB USER_SOURCES   ${CMAKE_CURRENT_SOURCE_DIR}/USER/*.c)
file(GLOB SYSTEM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/System/*.c)
file(GLOB APP_SOURCES    ${CMAKE_CURRENT_SOURCE_DIR}/SoftWare/src/*.c)
file(GLOB SPL_SOURCES    ${LIB_ROOT}/Libraries/STM32F4xx_StdPeriph_Driver/src/*.c)

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
