# FreeRTOS-Kernel (vendored)

**版本**: `V11.3.1`
**来源**: <https://github.com/FreeRTOS/FreeRTOS-Kernel> (官方仓库, tag `V11.3.1`)
**许可**: MIT (见 `LICENSE.md`)

> 官方 LTS 版本 **202604.00-LTS** 里带的内核是 v11.3.0, v11.3.1 是它的补丁版。
> LTS 的支持期到 2028-04-30。

## ⚠ 本目录是第三方代码，不要改

要调内核行为请改 **`../SoftWare/system/inc/FreeRTOSConfig.h`** —— 那才是本工程的
配置入口, 所有裁剪/优先级/堆大小都在那里。这里一个字都不要动,
将来升级内核时直接整目录替换即可。

## 为什么只有这些文件

从完整仓库里**只挑了 Cortex-M4F + GCC + heap_4 这一条路径**：

| 取了什么 | 为什么 |
|---|---|
| 根目录 7 个 `.c` | 内核本体(tasks/queue/list/timers/event_groups/stream_buffer/croutine) |
| `include/` | 全部内核头文件 |
| `portable/GCC/ARM_CM4F/` | **本工程用的移植层**。必须是 `ARM_CM4F`(带 FPU), 不是 `ARM_CM4` —— 工程的编译选项是 `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| `portable/MemMang/heap_4.c` | 堆实现。heap_4 支持碎片合并, 是最常用的那个 |

**没有取** `portable/` 下其它架构/编译器的目录(每个都有同名 `port.c`, 一起编译会
重复定义)、`examples/`、各种 SPDX 清单。

## 升级步骤

```bash
git clone --depth 1 --branch <新tag> https://github.com/FreeRTOS/FreeRTOS-Kernel.git /tmp/frk
# 按上表把对应文件覆盖过来, 然后
cmake -S . -B build -G Ninja    # ⚠ 必须重跑, file(GLOB) 不会自己发现新文件
```
