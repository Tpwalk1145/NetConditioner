# NetConditioner 弱网模拟器（Rust 单文件版）

Windows 系统级弱网模拟工具：通过全局热键一键开启/停止上行、下行的延时、抖动、带宽、随机丢包和连续丢包。界面参考弱网测试 App 的配置方式，做成“VPN 客户端”的连接/断开形式，但底层使用 **WinDivert 2.2.2** 在网络层拦截并重注入 TCP/UDP，不需要 TUN/TAP 虚拟网卡，也不会修改系统路由、DNS 或防火墙。

本仓库是 2.1.0 的 **Rust 单文件版**：只有一个 exe，内置 WinDivert 运行时，不需要安装包、不需要 .NET 运行时；首次运行会把内置的 WinDivert.dll / WinDivert64.sys 解压到本机运行时目录（约 140 KB），之后直接复用。

## 功能

- 单文件分发：整个程序就是一个 NetConditioner.exe，拷贝到任意目录双击即可运行
- 上行 / 下行独立配置：
  - 延时（ms）
  - 延时抖动（ms）
  - 带宽（kbps，0 = 不限速）
  - 随机丢包（0–100%）
  - 连续丢包：放行时长 + 丢包时长循环（ms）
- 弱网协议：TCP、UDP、ALL（TCP + UDP）
- 目标进程过滤（全局设置）：主窗口“启动弱网”上方可选择程序，只对它们生效；**不选任何进程 = 全局**
  - 按进程名匹配（如 chrome.exe），同一程序的多开实例都会生效
  - 支持按进程名搜索；勾选即生效
- 多套配置：新建、编辑、删除、快速切换
- 小窗口 + 系统托盘 + 自定义全局热键（默认 Ctrl + Alt + U；也支持单键，如 F8）
  - 单键会全局占用该按键，例如把字母 A 设为热键后，正常打字也会触发；建议优先用 F1–F12 等不常用键。
- 实时统计：上下行发送、丢弃、队列长度
- 云更新：配置更新地址后，后台静默检查、下载新 exe 并自替换重启（不弹窗，升级时窗口闪一下）
- 最长运行时间保护：到时自动停止并恢复网络
- 最大额外排队时延保护：避免突发流量把队列堆到几分钟，造成严重卡顿
- 配置与目标进程设置保存在 %AppData%\NetConditioner\config.json

## 一个重要限制：TCP 下载依赖上行 ACK

“上行完全为 0、下载不受影响”在 TCP 协议上不可能实现：客户端必须持续向上游发送 ACK，TLS/HTTP2/QUIC 还需要握手和控制包。因此：

- 上行带宽设得很低时，下载也会变慢；
- 上行随机丢包设为 100% 时，网页、下载、远程桌面都会中断；
- 本工具是网络条件模拟器，不会绕过这个协议事实。

如果目标是“限制大流量上传但尽量保留下载”，建议上行带宽从 32–128 kbps 开始试，而不是直接 0/100% 丢包。

## 环境要求

- Windows 10 / 11 x64
- 管理员权限（WinDivert 驱动需要）
- 运行程序：**不需要安装任何东西**，只需一个 NetConditioner.exe；首次运行会解压约 140 KB 的 WinDivert 运行时
- 构建程序：
  - Rust（MSVC 工具链，x86_64-pc-windows-msvc；本项目在 rustc 1.102 上开发验证）
  - Visual Studio 2022 Build Tools（“使用 C++ 的桌面开发”）+ Windows SDK（需要其中的 rc.exe 来嵌入版本信息与 manifest）
  - 单文件版不再需要 Inno Setup
- 随程序分发的 WinDivert 运行时（2.2.2 官方 DLL/驱动）已编进 exe

## 构建与运行

一键构建（编译 + 单元测试 + 生成单文件 exe 与更新清单）：

    cd NetConditioner
    .\build.ps1

也可以直接双击根目录的 build.cmd（等价于执行 build.ps1）。

只编译（并跑测试），不生成 dist 产物：

    .\build.ps1 -SkipPackage

跳过测试：

    .\build.ps1 -SkipTests

或者手动执行：

    cargo build --release
    cargo test

开发运行（不需要再手动拷贝 WinDivert 文件）：

    cargo build --release
    .\target\release\NetConditioner.exe

构建产物：

    dist\NetConditioner-2.1.0-win-x64.exe
    dist\latest.json

实测体积与启动时间（本机 Release 构建）：

| 项目 | 数值 |
| --- | --- |
| 单文件 exe | 约 2.1 MB |
| 首次运行解压的 WinDivert 运行时 | 约 140 KB（%ProgramFiles%\NetConditioner\runtime\2.2.2\） |
| 冷启动到窗口可见 | 约 50–100 ms（本机实测约 60 ms；首次运行含解压校验） |

首次运行会弹出 UAC；程序需要以管理员身份运行。WinDivert.dll 会从运行时目录加载，并在 DLL 同目录找到 WinDivert64.sys 后自动安装/加载驱动。

### 单文件是怎么做到的

- WinDivert.dll 与 WinDivert64.sys 通过 include_bytes! 编进 exe；
- 启动时优先解压到 %ProgramFiles%\NetConditioner\runtime\<WinDivert 版本>\（管理员独占可写，避免被普通用户替换），失败则依次回退到 %ProgramData%、%LOCALAPPDATA%；
- 每次启动都会校验已解压文件与内置内容是否一致，不一致就重写；
- 加载时使用 LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)，确保 WinDivert 从该目录加载并在同目录找到驱动；
- 旧版本运行时目录会被尽力清理。

注意：Windows 安装驱动必须有一个真实的 .sys 文件，因此“单 exe”指的是**分发只有一个文件**，运行时仍会落盘约 140 KB；这无法避免。

## 分发与云更新清单

单文件版不需要安装包，分发只需要：

1. dist\NetConditioner-<版本>-win-x64.exe
2. dist\latest.json（如果启用云更新）

build.ps1 常用参数：

| 参数 | 说明 |
| --- | --- |
| -Version 2.1.1 | 指定产物文件名与 latest.json 版本（默认取 Cargo.toml 的 version）；exe 内嵌版本始终来自 Cargo.toml，不一致时会警告 |
| -UpdateBaseUrl <url> | 云更新地址前缀；生成 latest.json 时用作更新包 URL（默认占位地址，上传前需替换） |
| -Notes <文本> | 写进 latest.json 的更新说明 |
| -Clean | 先删除 target / dist 再构建 |
| -SkipTests | 不运行 cargo test |
| -SkipPackage | 只编译（并跑测试），不生成 dist 产物 |
| -Configuration Debug | 指定构建配置（默认 Release） |

生成的 latest.json（schemaVersion 2，installer 字段指向单文件 exe）：

    {
      "schemaVersion": 2,
      "version": "2.1.1",
      "publishedAt": "2026-11-24T00:00:00Z",
      "notes": "修复若干问题",
      "pageUrl": "https://你的域名/NetConditioner/",
      "installer": {
        "url": "https://你的域名/NetConditioner/NetConditioner-2.1.1-win-x64.exe",
        "sha256": "D9C7F19E...",
        "size": 2215424
      }
    }

把 exe 和 latest.json 上传到同一目录即可。任意静态托管（对象存储、Nginx、GitHub Releases）都可以；用 GitHub Releases 时可以用固定地址 https://github.com/<用户>/<仓库>/releases/latest/download/latest.json。

## 云更新（静默自替换）

配置更新地址后，程序会在后台**完全静默**地检查、下载、校验并替换自身：没有弹窗、没有托盘气泡、没有安装向导，唯一可见的现象是升级时窗口闪一下后重新出现。

### 行为

- 启动约 5 秒后检查一次，之后每 6 小时检查一次；**每天最多检查一次**（距上次检查不足 24 小时则跳过）。
- 发现新版本后自动下载到 %TEMP%\NetConditioner\update\，边下边算 SHA256；校验失败会删除文件，并在 24 小时内不再尝试同一版本。
- 只有“空闲”时才替换：弱网未运行、没有正在切换、没有打开设置/编辑等对话框。弱网运行中只等待，不打断；停止弱网或隐藏到托盘后会继续。
- 替换方式：把当前 exe 复制成更新助手，助手等旧进程退出后用新 exe 覆盖原路径并重新启动；新文件会去掉 Mark-of-the-Web，避免被 SmartScreen 拦。
- 所有失败只写入 %TEMP%\NetConditioner\update\update.log，不打扰用户。
- exe 所在目录必须可写（不要放在只读目录/光盘）；只读时替换失败会保留当前版本并写日志。

### 配置

打开“设置 → 软件更新”：

| 项 | 说明 |
| --- | --- |
| 更新地址（latest.json） | 例如 https://你的域名/NetConditioner/latest.json；留空 = 关闭云更新（默认） |
| 自动静默更新 | 默认开启；关闭后不会检查更新 |

仅支持 https；本机回环地址（http://127.0.0.1:...）允许 http，便于本地联调。

### 诊断

    # 按当前配置检查一次，结果写到 %TEMP%\NetConditioner-update-check.log（不弹任何界面）
    NetConditioner.exe --check-update

    # 开发者联调：只记录更新命令，不真正替换/重启
    $env:NC_UPDATE_DRY_RUN = "1"

    # 导出 WinDivert 许可文本到 %TEMP%\NetConditioner\licenses\WinDivert-LICENSE.txt
    NetConditioner.exe --licenses

### 安全边界

自动更新没有用户确认环节，因此依赖：**https 传输 + latest.json 中的 SHA256 完整性校验 + 更新源可信**。请只把更新地址指向自己控制的服务器；项目未做代码签名。

latest.json 的 schemaVersion 为 2。旧版（安装包版，schemaVersion 1）会拒绝该清单，不会把新 exe 误当成安装程序执行。

## 从旧版迁移

- 如果之前装过 1.x / 2.0.0 的安装包版本：先卸载旧版（旧安装包仍在“应用和功能”里），再用单文件 exe；配置、目标进程、热键、更新设置都保留在 %AppData%\NetConditioner\config.json。
- 旧安装版不会自动升级到单文件版（更新清单版本号不同），需要手动替换一次。

## 使用

1. 点击“新建”，填写配置名称。
2. 设置上行/下行参数，选择协议（ALL 表示 TCP + UDP）。
3. 如需只对指定程序生效，在主窗口“目标进程”处点击“选择进程…”；留空则对所有程序生效。
4. 点击“启动弱网”。
5. 按全局热键或点击“停止并恢复”即可随时停止。
6. 关闭窗口会隐藏到托盘，弱网状态保持不变；从托盘菜单“退出”会停止并恢复网络。

### 参数语义

| 参数 | 说明 |
| --- | --- |
| 延时 | 数据包额外等待的时间 |
| 延时抖动 | 在 延时 + random(0..抖动) 范围内随机 |
| 带宽 | 0 = 不限速；按 kbps 串行化发送，从第一个包开始生效 |
| 随机丢包 | 每个命中协议的包按百分比独立丢弃 |
| 连续丢包-放行 | 连续丢包循环中的放行阶段时长 |
| 连续丢包-丢包 | 连续丢包循环中的丢包阶段时长 |

### 目标进程过滤

- 目标进程是主窗口“启动弱网”上方的全局设置，不属于某一套配置；切换配置不会改变目标进程。
- 最多可同时选择 64 个进程。
- **不选任何进程 = 全局**：全系统 TCP/UDP 都应用弱网参数。
- 选择进程后：引擎通过 Windows 的 TCP/UDP 表把本机连接映射到进程，只对命中进程名的流量应用延时/丢包/限速，其它程序的流量直接放行。
- 进程设置保存的是文件名（如 chrome.exe），不是 PID；程序重启、多开或稍后启动都能继续匹配。
- 映射每 2 秒刷新一次；新连接会按需刷新。连接刚建立的前几十毫秒可能还没被识别而直通，这是为了不影响其它程序。
- 只覆盖常规 IPv4/IPv6 的 TCP/UDP 包；带不常见 IPv6 扩展头或非首片分片的包会直通。
- 进程过滤模式每个包多一次本地端点查找，开销很小；极高流量下会比全局模式略高一点 CPU。
- 同一文件名的不同程序会被一起匹配；本工具按文件名匹配，不按完整路径，无法区分同名程序。

### 突发流量与卡顿

- 引擎复用包缓冲区，发送线程按批处理，避免每个包分配非托管内存和频繁唤醒。
- WinDivert 内核队列为 16384 个包 / 32MB，给用户态处理留出缓冲。
- “设置”里的“最大额外排队时延”默认 2000ms：只限制超出配置延时之外的排队。超过上限的包会被丢弃（计入“丢弃”），防止低带宽 + 大突发把队列堆到几分钟，导致系统看起来卡死。重启弱网后生效。
- 设为 0 表示不限制排队：突发时更不容易丢包，但可能出现明显延迟甚至长时间卡顿。极端突发下，任何限速器都必须在“丢包”和“高延迟”之间做取舍。
- 64KB / 1MB 突发（10ms 延时、不限速）均无额外丢包（自测覆盖）。

连续丢包规则：

- 两个值都为 0：关闭；
- 只填“丢包”：一直丢包；
- 只填“放行”：不丢包；
- 两个都填：放行 → 丢包 → 放行循环。

### 配置示例

**只限制上行带宽（最接近“上行带宽开关”）**

    上行带宽：64 kbps
    其余全部为 0
    协议：ALL

**模拟高延迟链路**

    上行延时：300 ms，抖动：100 ms
    下行延时：300 ms，抖动：100 ms

**模拟上行丢包**

    上行随机丢包：50%

**完全阻断上行（注意：下载也会停）**

    上行随机丢包：100%

## 配置文件兼容性

config.json 与 1.x/2.0.0 完全兼容：camelCase 属性名、协议枚举写成字符串（Tcp / Udp / All）、字段缺失或为 null 时回退默认值。升级到单文件版后原有配置、目标进程、热键、更新设置都会保留；Rust 版与旧 .NET 版写出的配置可互相读取（已实测双向读取）。

## 安全与恢复

- 弱网只在程序运行期间生效。
- 停止、退出程序或程序崩溃时，WinDivert 句柄会被关闭，网络立即恢复正常。
- 不修改系统路由、DNS、代理、防火墙或网卡配置。
- WinDivert 驱动会由 DLL 自动安装/加载；驱动服务本身会保留在系统中，这是 WinDivert 的正常行为。
- 如果远程桌面中设置了高丢包/低带宽，可能断开连接。请先在“设置”里设置“最长运行时间”，让程序到时自动恢复。
- 单文件版会在 %ProgramFiles%\NetConditioner\runtime\ 留下约 140 KB 的运行时文件；删除 exe 后如需彻底清理，可一并删除该目录和 %AppData%\NetConditioner（配置）。

## 故障排查

| 现象 | 处理 |
| --- | --- |
| 启动失败，错误 2 | WinDivert 运行时文件不完整；删除 %ProgramFiles%\NetConditioner\runtime 后重试 |
| 提示“无法准备 WinDivert 运行时” | 程序无法写入 Program Files / ProgramData / AppData；请以管理员身份运行或检查磁盘权限 |
| 启动失败，错误 5 | 没有以管理员身份运行 |
| 启动失败，错误 577 / 1275 | 驱动签名被拦截或安全软件阻止；检查杀毒软件，使用随包的 2.2.2 官方文件 |
| 提示已有不兼容的 WinDivert 驱动 | 本机其它程序可能加载了不同版本的 WinDivert；退出该程序或重启后再试 |
| 被杀软误报 | 单文件版会在运行时释放签名驱动并加载，部分安全软件会敏感；可加入白名单或改用旧安装版 |
| 热键没反应 | 热键被其它程序占用，打开“设置”换一个组合 |
| 选了目标进程但没效果 | 确认进程名与任务管理器一致；新启动的程序最多约 2 秒后生效；系统驱动级流量可能没有 TCP/UDP 记录 |
| 启动后立即恢复 | 检查 Windows 事件查看器/杀毒软件；可运行 NetConditioner.exe --self-test 查看日志 |
| 云更新没生效 | 看 %TEMP%\NetConditioner\update\update.log；确认“设置 → 软件更新”里已填 https 地址并勾选自动静默更新 |
| 更新后程序没重新打开 | 手工启动程序；更新日志在 %TEMP%\NetConditioner\update\update.log |
| 更新失败提示无法替换 | exe 所在目录不可写（如在只读共享/受保护目录）；把 exe 放到用户可写目录 |
| 构建报错找不到 rc.exe | 安装 Visual Studio 2022 Build Tools（C++ 桌面开发）与 Windows SDK，或在 “Developer PowerShell” 里执行构建 |

自测：

    NetConditioner.exe --self-test

覆盖上下行延时、64kbps 限速、100% 丢包、停止恢复、进程过滤（IPv4/IPv6 TCP、UDP；命中生效 / 未命中直通）、64KB/1MB 突发无额外丢包、真实网卡 80 端口的上下行方向验证，以及云更新（版本比较、latest.json 校验、配置清洗、本地回环 HTTP 下载与 SHA256 校验/篡改拒绝、自更新 dry-run）。结果写入 %TEMP%\NetConditioner-selftest.log。自测只影响本机回环测试端口和公网 80 端口，不会影响 443 连接。Release 版同样支持 --self-test（已实测全项通过）。

## 与 TUN/TAP、VPN 的关系

本工具不创建 TUN/TAP 虚拟网卡，界面按 VPN 客户端风格设计。Windows 上做系统级弱网模拟的标准方式就是 WinDivert（与 Clumsy 同类）；TUN/TAP 只负责把流量导入用户态，真正做延时/丢包/限速同样需要包处理引擎，而且自建路由/NAT 更容易把本机网络弄断。

## 目录结构

    NetConditioner/
      Cargo.toml                  # 包信息、依赖、Release 优化配置
      build.rs                    # 生成并嵌入 VERSIONINFO 与 app.manifest
      app.manifest                # requireAdministrator + Win10/11 supportedOS
      build.ps1 / build.cmd       # 一键构建（编译 + 测试 + 单文件产物 + latest.json）
      src/
        main.rs                   # 入口、参数、单实例、管理员校验、DPI、消息泵
        app.rs                    # 应用状态、主窗口消息处理、启停/配置/更新流程
        win.rs                    # Win32 薄封装（窗口、消息、GDI、字体、托盘基础）
        config/                   # 配置模型、清洗规则、原子读写
        engine/                   # WinDivert 内嵌运行时/绑定、包解析、进程过滤、方向引擎
        ui/                       # 主窗口、配置编辑、设置、进程选择、托盘、自绘控件
        update/                   # 云更新（检查/下载/校验/自替换/状态机）
        selfupdate.rs             # 自更新助手（等旧进程退出后替换 exe 并重启）
        licenses.rs               # 导出 WinDivert 许可文本
        hotkey.rs version.rs neturl.rs selftest.rs checkupdate.rs
      third_party/WinDivert/      # WinDivert 2.2.2 DLL/SYS、LICENSE、VERSION（编进 exe）
      README.md

## 许可

WinDivert 由 Basil（reqrypt.org）开发，采用 LGPLv3 / GPLv2 双许可。本项目动态加载未修改的 WinDivert DLL/驱动（单文件版把它们内嵌并在运行时解压加载），许可全文见 third_party/WinDivert/LICENSE，也可以运行 NetConditioner.exe --licenses 导出。

WinDivert 源码：https://github.com/basil00/WinDivert
