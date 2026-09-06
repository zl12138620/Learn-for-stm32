# pyOCD 器件包（CMSIS-Pack）下载与使用说明

> 适用芯片：STM32F407VGT6 · 调试器：CMSIS-DAP（野火 FireDAP）
> 配套文档：`README-CMAKE.md`（构建/调试总指南）

## 1. 什么是器件包？为什么需要它？

pyOCD 内置的器件 target 有限（本机约 302 个，**不含 STM32F4 系列**）。
要让 pyocd 认识 STM32F407VG，需要给它一个 **CMSIS Device Family Pack（.pack 文件）**——
里面含器件内存布局、Flash 编程算法、SVD 等描述。
没有它就会出现：

```
PyOCD: GDB Server Quit Unexpectedly     # cortex-debug 里选 pyocd 配置时报
```

## 2. 去哪里下载？（官方来源）

pyOCD 的器件包来自 **Keil 官方 CMSIS-Pack 服务器**（也是 Keil MDK 的同一来源）：

| 项目 | 内容 |
|---|---|
| 服务器 | `https://keilpack.azureedge.net/pack/` |
| 包名规则 | `<Vendor>.<Name>.<Version>.pack` |
| 本芯片对应包 | `Keil.STM32F4xx_DFP`（内容由 **STMicroelectronics** 提供） |
| 当前版本 | 3.1.1（用下面命令随时可查最新版） |
| 直链 | `https://keilpack.azureedge.net/pack/Keil.STM32F4xx_DFP.3.1.1.pack` |

查询最新可用版本（需联网）：

```powershell
pyocd pack find stm32f407vg
```

输出示意：
```
Part              Vendor               Pack                 Version   Installed
STM32F407VGTx   STMicroelectronics   Keil.STM32F4xx_DFP   3.1.1     False
```

> 其它来源：`https://www.keil.com/pack/`（Keil Pack 网页目录）也是同一批文件；
> 部分 STM32 旧型号也可能出现在 ST 官网的 STM32Cube 下载中心。

## 3. 本工程已下载好的器件包

| 项 | 值 |
|---|---|
| 位置 | `pyocd/Keil.STM32F4xx_DFP.3.1.1.pack`（工程根目录下） |
| 大小 | 2,085,496 字节 |
| 校验 | 服务器 Content-MD5(Base64)：`ibWhdtvoqI6FWCYoWEmFWg==` |

如需手动重新下载（2 秒即可）：

```powershell
# 方式 A：命令行 curl（本工程已用此方式）
cd C:\Users\30976\Desktop\学习\STM32F407VGT6
curl.exe -L -o pyocd\Keil.STM32F4xx_DFP.3.1.1.pack `
  https://keilpack.azureedge.net/pack/Keil.STM32F4xx_DFP.3.1.1.pack

# 方式 B：浏览器直接打开上面的直链，把文件存到 pyocd/ 目录
```

## 4. 怎么用？（三种方式任选）

### 4.1 VS Code + Cortex-Debug（本工程已配置好）

**注意**：Cortex-Debug 1.12.1 **不支持 `cmsisPack` 字段**——它启动 pyocd 时不会自动
加载工程目录里的 `.pack`。所以要让 Cortex-Debug 的 pyOCD 配置可用，必须先把器件包
**安装进 pyocd 本体**（见 4.3），然后 launch.json 只需普通配置：

```jsonc
{
  "name": "Cortex-Debug (pyOCD / CMSIS-DAP)",
  "type": "cortex-debug",
  "request": "launch",
  "servertype": "pyocd",
  "targetId": "stm32f407vg",
  "executable": "${workspaceFolder}/build/output/STM32F407VGT6.elf",
  "runToEntryPoint": "main",
  "preLaunchTask": "build"
}
```

本工程已执行过 `pyocd pack install stm32f407vg`（装入 pyocd 用户目录），
`.vscode/launch.json` 已配好，选该配置按 F5 即可。

### 4.2 pyocd 命令行临时使用（`--pack` 参数）

```powershell
# 烧录
pyocd flash -t stm32f407vg --pack pyocd\Keil.STM32F4xx_DFP.3.1.1.pack `
           build\output\STM32F407VGT6.hex --reset

# 调试会话（gdbserver）
pyocd gdbserver -t stm32f407vg --pack pyocd\Keil.STM32F4xx_DFP.3.1.1.pack
```

### 4.3 安装进 pyocd 用户目录（推荐 / 本工程已执行）

pyocd 0.40 没有“本地导入 .pack”的子命令（`pyocd pack` 只有
clean/find/install/show/update），所以标准做法是**按器件型号从官方索引下载安装**：

```powershell
pyocd pack install stm32f407vg      # 自动下载 Keil.STM32F4xx_DFP 到 %USERPROFILE%\.pyocd
pyocd list --targets | findstr /i 407   # 确认已安装（来源列显示 pack）
```

安装后 `-t stm32f407vg` / Cortex-Debug 都不再需要 `--pack`。
本工程已执行过该命令（当前已装 `Keil.STM32F4xx_DFP 3.1.1`）。
注意：这是装在 pyocd 用户目录（`%USERPROFILE%\.pyocd`），换电脑后需重新执行；
工程 `pyocd/` 目录里的 `.pack` 文件可当作离线备份/给命令行 `--pack` 用。

## 5. 验证本工程器件包是否可用

```powershell
# 已安装到 pyocd（无需 --pack）
pyocd list --targets | findstr /i 407v

# 用工程内 .pack 文件临时验证（命令行 --pack 场景）
pyocd list --targets --pack pyocd\Keil.STM32F4xx_DFP.3.1.1.pack | findstr /i 407v
```

应能看到 `stm32f407vg` / `stm32f407vgtx` 等条目（来源列显示 `pack`）。

## 6. 更新器件包版本

1. `pyocd pack find stm32f407vg` 查最新版本号（例如变成 3.2.0）；
2. 若要换用新版本：`pyocd pack update` 刷新索引后再
   `pyocd pack install stm32f407vg`（会自动装最新版）；
3. 命令行 `--pack` 场景：把第 3 节 URL 里的 `3.1.1` 换成新版本号下载到
   `pyocd/` 目录即可（Cortex-Debug 调试走的是 pyocd 已安装的包，无需改 launch.json）。

## 7. 常见问题

| 现象 | 处理 |
|---|---|
| `--pack` 路径带空格/中文报错 | 用双引号包住路径 |
| 加了 pack 仍报 target 找不到 | 确认器件确实在该 pack 内：`pyocd list --targets --pack ... | findstr 407` |
| FireDAP 识别为 n/a / 无 probe | 硬件链路问题，先跑 `pyocd list`；与器件包无关 |
| cortex-debug 报 `Failed to launch PyOCD GDB Server: Timeout.`（器件包已装仍复现） | Cortex-Debug 1.12.1 与 pyocd 0.36+ 的已知兼容 bug（官方 issue #1222）。升级扩展到 **≥1.13.0-pre10**：`code --install-extension tools\cortex-debug-1.13.0-pre10.vsix --force`（本工程已下载 VSIX 到 `tools/`），随后 Reload Window |
| cortex-debug 报 `Failed to launch PyOCD GDB Server: Timeout.`（器件包未装） | 先 `pyocd pack install stm32f407vg`，再 `pyocd list --targets \| findstr 407v` 确认，然后重按 F5 |
| cortex-debug 里想指定本地 pack | Cortex-Debug 1.12.1 不支持 `cmsisPack` 字段；用 `pyocd pack install` 装入 pyocd，或命令行加 `--pack`（见 4.2） |
