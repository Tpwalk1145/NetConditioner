# NetConditioner

Windows 系统级弱网模拟工具，基于 WinDivert 为 TCP/UDP 上下行流量模拟延时、抖动、带宽限制、随机丢包与连续丢包。

项目采用 **C23 + 原生 Win32 API** 编写，零第三方运行库依赖；默认使用完整 LLVM 工具链（`clang-cl` / `lld-link` / `llvm-rc`）编译为单个 x64 可执行文件，内嵌 WinDivert 运行时，同时保留 MSVC 回退构建选项。

## 特性一览

- **单文件便携**：内嵌 WinDivert 2.2.2 DLL 与驱动，双击即可使用，无需安装运行库。
- **精细控制**：支持多套配置方案、独立上下行参数，以及 `TCP` / `UDP` / `ALL` 协议过滤。
- **进程级过滤**：留空时全局生效，或指定最多 64 个目标进程（按进程名匹配，如 `chrome.exe`）。
- **安全防护**：支持最长运行时间自动停止、最大额外排队时延上限与全局紧急停止热键（默认 `Ctrl + Alt + U`）。
- **托盘与统计**：关闭窗口自动最小化到系统托盘，实时展示收发与丢包统计。
- **静默云更新**：支持通过 `latest.json` 自动检查更新，后台下载并校验 SHA256 与文件大小，空闲时安全替换并重启。

## 快速开始

### 系统要求

- Windows 10 / 11 x64
- 管理员权限（启动时通过 UAC 请求）

### 运行步骤

1. 从 [Releases](https://github.com/Tpwalk1145/NetConditioner/releases) 下载最新版本，或按[从源码构建](#从源码构建)自行编译。
2. 将 `NetConditioner.exe` 放入可写目录并双击运行。
3. 新建或编辑配置方案，设置上下行弱网参数和目标协议。
4. 如需只作用于特定程序，在 **目标进程** 中勾选对应进程；留空则对全局流量生效。
5. 点击 **启动弱网** 开始模拟；随时点击 **停止并恢复** 或按下全局热键（默认 **Ctrl + Alt + U**）恢复网络。
6. 关闭主窗口会隐藏到系统托盘；在托盘菜单选择 **退出** 会先停止弱网并恢复网络，再退出程序。

> **升级说明**：从 `2.11.0` 升级到 C 重写版可直接替换 exe，配置文件、热键、目标进程与更新清单格式完全兼容。从早期 `1.x` / `2.0.0` 安装包版升级时，请先卸载旧版再运行单文件 exe。建议升级前备份 `config.json`。

### 内嵌运行时释放路径

首次启用弱网时，程序会将内嵌的 `WinDivert.dll` 与 `WinDivert64.sys` 释放到以下目录（若不可写则依次回退）：

1. `%ProgramFiles%\NetConditioner\runtime\<WinDivert版本>\`
2. `%ProgramData%\NetConditioner\runtime\<WinDivert版本>\`
3. `%LOCALAPPDATA%\NetConditioner\runtime\<WinDivert版本>\`

## 参数说明

| 参数 | 范围 | 默认值 | 说明 |
| --- | --- | --- | --- |
| 延时 | `0–60000 ms` | `0` | 数据包额外等待的时间 |
| 延时抖动 | `0–60000 ms` | `0` | 实际等待时间在 `延时 + random(0..=抖动)` 范围内随机 |
| 带宽 | `0–1000000 kbps` | `0` | `0` 表示不限速 |
| 随机丢包 | `0–100%` | `0` | 每个命中协议的数据包按设定百分比独立丢弃 |
| 连续丢包 - 放行 | `0–600000 ms` | `0` | 突发丢包周期内的放行阶段时长 |
| 连续丢包 - 丢包 | `0–600000 ms` | `0` | 突发丢包周期内的丢包阶段时长 |
| 最长运行时间 | `0–1440 分钟` | `0` | 到时自动停止并恢复网络（适用于远程桌面等防断连场景）；`0` 表示不自动停止 |
| 最大额外排队时延 | `0–60000 ms` | `2000` | 限制排队导致的额外时延堆积；`0` 表示不限制 |

**连续丢包规则**：

- 放行 = `0` 且 丢包 = `0`：关闭连续丢包。
- 仅填“丢包”：持续丢包。
- 仅填“放行”：不丢包。
- 两者均大于 `0`：按 **放行 → 丢包** 周期循环。

> **注意**：TCP 下载依赖上行 ACK，因此上行限速或丢包也会显著影响下载速度；实时统计中的“发送”计数包含直通放行的数据包。

## 目标进程过滤

- 留空时对系统全局 TCP/UDP 流量生效（本地回环流量除外）。
- 最多同时选择 **64** 个进程，按进程文件名匹配（如 `chrome.exe`），同名进程会一起匹配。
- 通过 Windows TCP/UDP 连接表映射连接与进程：每 2 秒定时刷新一次，遇到新连接也会按需刷新。
- 受保护进程与部分系统流量可能无法获取 Owner PID；新建连接在映射刷新前可能短暂直通。

## 配置、停止与清理

### 配置文件

默认保存在 `%APPDATA%\NetConditioner\config.json`（camelCase JSON 格式）：

- 加载时自动清洗越界数值，缺失或为 `null` 的字段自动回退默认值。
- 文件损坏时会自动备份并回退到默认配置。
- 保存时会保留配置中的未知字段，便于跨版本兼容。

### 停止、恢复与彻底清理

- 点击 **停止并恢复**、从托盘退出或进程退出时，程序会关闭 WinDivert 句柄并尽快尝试重新注入排队中的数据包（发送失败或线程异常时丢弃剩余包）。
- WinDivert 驱动服务会保留在系统中以供下次快速启动。
- 如需彻底清理程序产生的文件，可按需删除以下目录：

```text
%ProgramFiles%\NetConditioner\runtime\
%ProgramData%\NetConditioner\runtime\
%LOCALAPPDATA%\NetConditioner\runtime\
%LOCALAPPDATA%\NetConditioner\update\
%APPDATA%\NetConditioner\              # 保存配置、目标进程、热键和更新设置
```

## 云更新与命令行选项

### 云更新

在 **设置 → 软件更新** 中填写 `latest.json` 地址并启用自动更新（默认开启；地址留空则关闭云更新）：

- **安全策略**：仅允许 HTTPS 地址（本机回环地址允许 HTTP 以便调试），HTTPS 使用 WinHTTP/Schannel 与 Windows 系统证书校验。请仅填写可信来源。
- **更新周期**：检查成功或同版本下载失败后的冷却间隔均为 **24 小时**。
- **静默升级**：后台下载并按 `latest.json` 校验 SHA256 与文件大小；当弱网已停止且界面空闲时，由更新助手覆盖当前 exe 并自动重启。

日志路径：

- 后台自动更新日志：`%LOCALAPPDATA%\NetConditioner\update\update.log`
- 命令行手动检查日志：`%TEMP%\NetConditioner-update-check-<随机id>.log`

### 命令行选项

| 命令 | 权限 | 说明 |
| --- | --- | --- |
| `NetConditioner.exe --check-update` | 普通 | 按当前配置检查一次更新，并将结果写入临时日志 |
| `NetConditioner.exe --licenses` | 普通 | 导出内嵌的 WinDivert 许可文本至 `%TEMP%\NetConditioner\licenses\WinDivert-LICENSE-<随机id>.txt` |
| `NetConditioner.exe --self-test` | 管理员 | 运行端到端自测（含 WinDivert 加载、配置、热键、更新/dry-run、上下行延时、进程过滤、带宽、丢包、恢复与真实网卡方向验证），日志写入 `%TEMP%\NetConditioner-selftest.log` |

开发联调自更新流程时，可通过环境变量启用 dry-run（不实际覆盖原 exe）：

```powershell
$env:NC_UPDATE_DRY_RUN = "1"
```

## 故障排查

| 现象 | 排查建议 |
| --- | --- |
| 启动失败，错误 2 | 删除 `%ProgramFiles%\NetConditioner\runtime` 后重试，并检查 `ProgramData` / `LOCALAPPDATA` 回退目录 |
| 提示“无法准备 WinDivert 运行时” | 检查运行时目录的磁盘空间与写入权限 |
| 启动失败，错误 5 | 请以管理员身份运行 |
| 错误 577 / 1275 | 检查系统驱动签名策略与安全软件拦截 |
| 错误 1058 | 检查 WinDivert 驱动服务状态与安全软件拦截 |
| 提示已有不兼容的 WinDivert 驱动 | 退出其他加载了不同版本 WinDivert 的程序，或重启系统 |
| 全局热键无响应 | 打开 **设置** 更换不冲突的快捷键 |
| 目标进程过滤不生效 | 核对进程名是否与任务管理器一致；新建连接需等待短暂映射刷新 |
| 启动后立即自动恢复 | 查看自测日志、Windows 事件查看器及安全软件拦截记录 |
| 云更新未生效 / 无法替换 exe | 确认已填写有效 HTTPS 地址且程序位于可写目录，检查 `update.log` |
| 更新后未自动重启 | 手动启动程序并检查 `update.log` |
| 构建找不到 `clang-cl` / `lld-link` / `llvm-rc` / `cl` | 安装完整 LLVM 并将 `bin` 加入 `PATH`，或改用 `-Toolchain MSVC`（需安装 VS 2022 Build Tools 与 Windows SDK） |

## 从源码构建

### 环境依赖

- Windows 10 / 11 x64
- **完整 LLVM/Clang**（需将 `clang-cl.exe`、`lld-link.exe`、`llvm-rc.exe` 加入 `PATH`；已验证版本 23.1.6）
- **Visual Studio 2022 Build Tools**（含“使用 C++ 的桌面开发”工作负载，提供 Windows 头文件与库；使用 `-Toolchain MSVC` 时也提供 `cl.exe` / `link.exe`）
- **Windows SDK**（提供系统库；MSVC 回退时提供 `rc.exe`）
- Windows PowerShell 5.1 或 PowerShell 7（`build.ps1` 以 UTF-8 BOM 保存）

### 常用命令

```powershell
# 默认 Release 构建（LLVM 工具链 + 单元测试 + 生成单文件 exe 与 latest.json）
.\build.ps1

# 仅编译并运行单元测试，不打包 dist 产物
.\build.ps1 -SkipPackage

# 跳过单元测试
.\build.ps1 -SkipTests

# Debug 构建（仅输出到 dist\debug\，不生成 latest.json）
.\build.ps1 -Configuration Debug -SkipTests

# 使用 MSVC 工具链回退构建
.\build.ps1 -Toolchain MSVC
```

也可以直接运行 `build.cmd`（支持透传上述所有参数）。自动化场景默认不暂停；若双击运行希望保留窗口，请加上 `-Pause`：

```bat
build.cmd -Pause
```

构建说明：

- 脚本会自动通过 `vswhere` 定位 VS 2022 的 `vcvars64.bat`，并校验最终可执行文件的 PE Machine 为 x64。
- LLVM 模式会并行调用 `clang-cl`（最多 `min(CPU 核数, 12)` 个进程）；MSVC 模式使用 `/MP`。
- 单元测试产物为 `build\tests\NetConditionerTests.exe`，无需管理员权限、不联网、不改动真实配置。

### 构建参数

| 参数 | 说明 |
| --- | --- |
| `-Toolchain <LLVM\|MSVC>` | 选择工具链，默认 `LLVM`（`Clang` 为兼容别名）；`MSVC` 回退到 `cl.exe` / `link.exe` / `rc.exe` |
| `-Configuration <Release\|Debug>` | 构建配置，默认 `Release`；`Debug` 输出到 `dist\debug\` 且不生成 `latest.json` |
| `-Version <版本>` | 可选；若指定则必须与 `version.txt` 完全一致 |
| `-UpdateBaseUrl <url>` | `latest.json` 中的安装包下载 URL 前缀 |
| `-PageUrl <url>` | `latest.json` 中的发布页地址 |
| `-Notes <文本>` | `latest.json` 中的更新说明 |
| `-Clean` | 构建前清理 `build` 和 `dist` 目录 |
| `-SkipTests` | 跳过单元测试 |
| `-SkipPackage` | 仅编译与测试，不生成 `dist` 发布产物 |
| `-Pause` | 仅供 `build.cmd` 使用，执行结束后暂停等待按键 |

### 版本管理与发布流程

1. **同步版本号**：修改根目录 `version.txt`（2–4 段规范整数，无前导零）并同步更新 `app.manifest` 的四段 `assemblyIdentity.version`（如 `2.12.0` 对应 `2.12.0.0`），`build.ps1` 会在编译前强校验两者一致。
2. **构建与校验**：在干净工作区运行 `.\build.ps1`（不要跳过测试），确认输出产物：
   - `dist\NetConditioner-<版本>-win-x64.exe`
   - `dist\latest.json`
3. **精确打标**：为构建对应的精确提交打注解标签并推送，再上传同一构建的 exe 与 `latest.json` 到同一 GitHub Release：

   ```powershell
   git rev-parse HEAD
   git tag -a v2.12.0 <完整提交SHA> -m "NetConditioner v2.12.0"
   ```

> **签名与标签提示**：当前构建不对 exe 进行 Authenticode 签名（未签名程序可能触发 SmartScreen 提示，`requireAdministrator` 仅用于申请管理员权限）。如需签名，请在构建后对 exe 签名，并用签名后的文件重新计算写入 `latest.json` 的 SHA256 与大小。已发布的版本标签切勿随意移动，以免影响现有用户的云更新校验。

## 项目结构

```text
src/
├── main.c              入口与命令行参数分流
├── app.c               应用状态机与主窗口消息循环
├── win.c               Win32 API 薄封装
├── util.c              UTF-8/UTF-16 转换、字符串、路径、时间与哈希
├── json.c              JSON DOM 解析与序列化
├── config.c            配置模型、清洗与校验
├── store.c             配置文件读写、原子写入与损坏恢复
├── hotkey.c            全局热键校验与格式化
├── version.c           版本号解析与比较
├── neturl.c            URL 安全校验、脱敏与 SHA256 规范化
├── licenses.c          内嵌许可文本导出
├── selftest.c          管理员端到端自测
├── engine/             弱网引擎（WinDivert 加载、方向队列、流解析、进程过滤）
├── ui/                 原生 Win32 界面（主窗口、控件、托盘、配置编辑、进程选择、设置）
└── update/             云更新检查、协调调度、WinHTTP 下载与自更新助手
tests/                  离线单元测试（无需管理员权限）
third_party/WinDivert/  随包内嵌的 WinDivert 2.2.2 二进制与许可
```

## 许可证与致谢

本程序动态加载并随包分发由 Basil 开发的 [WinDivert](https://github.com/basil00/WinDivert)，其采用 **LGPLv3 或 GPLv2** 双许可协议。完整许可文本见 [third_party/WinDivert/LICENSE](third_party/WinDivert/LICENSE)，也可通过运行 `NetConditioner.exe --licenses` 导出查看。
