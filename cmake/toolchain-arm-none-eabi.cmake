# ==============================================================================
# toolchain-arm-none-eabi.cmake
# Arm GNU GCC (arm-none-eabi) 交叉编译工具链配置 —— 适用于 STM32F407VGT6 裸机工程
# CMake cross toolchain file for STM32 (Cortex-M) with arm-none-eabi-gcc.
# ==============================================================================

# 裸机目标：无操作系统
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m4)

# 避免 CMake 在配置阶段执行链接测试（嵌入式目标没有可运行环境）
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 工具链前缀（若工具不在 PATH，可在此指定绝对前缀，例如：
#   set(TOOLCHAIN_PREFIX "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/13.3.Rel1/bin/arm-none-eabi-")
if(NOT DEFINED TOOLCHAIN_PREFIX)
  set(TOOLCHAIN_PREFIX arm-none-eabi-)
endif()

# 编译器 / 工具
set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_SIZE         "${TOOLCHAIN_PREFIX}size")
set(CMAKE_OBJDUMP      "${TOOLCHAIN_PREFIX}objdump")

# 交叉编译时查找策略：
#  - 程序仍在主机 PATH 查找（如 ninja）
#  - 头文件 / 库只在目标 sysroot 查找
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 统一使用 arm-none-eabi GCC 的开关（供需要切换编译器时使用）
option(USE_ARMGCC "Use Arm GNU GCC toolchain (arm-none-eabi)" ON)
