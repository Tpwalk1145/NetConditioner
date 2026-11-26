# NetConditioner

Windows 系统级弱网模拟工具，使用 WinDivert 为 TCP/UDP 上下行模拟延时、抖动、带宽限制、随机丢包和连续丢包。

程序使用 C17 + 原生 Win32 实现，默认使用完整 LLVM 工具链（clang-cl 编译、lld-link 链接、llvm-rc 编译资源）生成单文件 exe；也保留 MSVC 工具链选项。

## 下载与运行

从 [Releases](https://github.com/Tpwalk1145/NetConditioner/releases) 下载最新版本，或按[构建与发布](#构建与发布)自行构建。

- 系统要求：Windows 10 / 11 x64、管理员权限。
- 将 exe 放到可写目录后双击运行。程序会通过 UAC 请求管理员权限。

WinDivert DLL 和驱动已内嵌。首次启用弱网时，程序会把它们释放到：

~~~
%ProgramFiles%\NetConditioner\runtime\<版本>\
~~~

Program Files 不可写时，依次尝试 %ProgramData% 和 %LOCALAPPDATA% 下的相同结构。

从 2.11.0 升级到 C 版时可以直接替换 exe：配置文件、热键、目标进程和更新清单格式保持不变，升级前建议备份 config.json。

## 快速开始

1. 启动 NetConditioner，新建一套配置。
2. 设置上行、下行参数和协议。
3. 如需限定程序，在“目标进程”中选择进程；留空表示全局生效。
4. 点击“启动弱网”。
5. 点击“停止并恢复”或按全局热键停止，默认热键为 **Ctrl + Alt + U**。
6. 关闭窗口会隐藏到托盘；从托盘菜单选择“退出”会停止弱网并退出程序。

## 功能与参数

支持多套配置、独立上下行设置、TCP/UDP/ALL 协议、目标进程过滤、托盘运行、实时统计、最长运行时间和最大额外排队时延。

| 参数 | 范围 | 说明 |
| --- | --- | --- |
| 延时 | 0–60000 ms | 数据包额外等待的时间 |
| 延时抖动 | 0–60000 ms | 在 延时 + random(0..=抖动) 范围内随机 |
| 带宽 | 0–1000000 kbps | 0 表示不限速 |
| 随机丢包 | 0–100% | 每个命中协议的包按百分比独立丢弃 |
| 连续丢包-放行 | 0–600000 ms | 循环中的放行阶段时长 |
| 连续丢包-丢包 | 0–600000 ms | 循环中的丢包阶段时长 |
| 最长运行时间 | 0–1440 分钟 | 0 表示不自动停止 |
| 最大额外排队时延 | 0–60000 ms | 默认 2000 ms；0 表示不限制额外排队 |

连续丢包规则：

- 两个值都为 0：关闭。
- 只填“丢包”：一直丢包。
- 只填“放行”：不丢包。
- 两个都填：按“放行 → 丢包”循环。

TCP 下载依赖上行 ACK，上行限速或丢包也会影响下载。实时统计中的“发送”包含直通放行的数据包。

## 目标进程过滤

- 不选择进程时，对全局 TCP/UDP 流量生效。
- 最多选择 64 个进程，按进程文件名匹配，例如 chrome.exe。
- 程序通过 Windows TCP/UDP 表把连接映射到进程，映射表每 2 秒刷新一次，新连接也会按需刷新。
- 同名程序会一起匹配；受保护进程和部分系统流量可能缺少 Owner PID。
- 连接刚建立时可能短暂直通。

## 停止与恢复

- 点击“停止并恢复”、从托盘退出或程序退出时，会关闭 WinDivert 句柄并停止弱网。
- 停止时会尽快尝试重新注入已排队的数据包；发送失败或线程异常时，剩余数据包会被丢弃。
- WinDivert 驱动服务会保留在系统中。

彻底清理时，可按需删除：

~~~
%ProgramFiles%\NetConditioner\runtime\
%ProgramData%\NetConditioner\runtime\
%LOCALAPPDATA%\NetConditioner\runtime\
%APPDATA%\NetConditioner\
~~~

最后一个目录保存配置、目标进程、热键和更新设置。

## 配置文件

默认路径：

~~~
%APPDATA%\NetConditioner\config.json
~~~

配置使用 camelCase JSON。加载时会清洗超出范围的数值，并为缺失字段或 null 使用默认值；损坏的配置会备份，程序回退到默认配置。未知字段在保存时会保留。

从 1.x / 2.0.0 安装包版升级时，先卸载旧安装版，再运行单文件 exe。配置路径保持不变，升级前建议备份 config.json。

## 云更新

打开“设置 → 软件更新”，填写 latest.json 地址并开启自动更新。自动更新开关默认开启，更新地址留空时关闭云更新。

程序会定期检查更新；成功检查或下载失败后，下次尝试间隔为 24 小时。发现新版本后，会在后台下载并按清单校验 SHA256 和大小；弱网停止且界面空闲时，由更新助手覆盖原 exe 并重启。

更新地址支持 HTTPS，本机回环地址允许 HTTP。HTTPS 使用 Windows 系统证书验证与 WinHTTP/Schannel。

日志位置：

~~~
%LOCALAPPDATA%\NetConditioner\update\update.log
%TEMP%\NetConditioner-update-check-<随机id>.log
~~~

按当前配置检查更新：

~~~
NetConditioner.exe --check-update
~~~

开发联调时启用 dry-run：

~~~
$env:NC_UPDATE_DRY_RUN = "1"
~~~

导出 WinDivert 许可文本：

~~~
NetConditioner.exe --licenses
~~~

许可文本保存到 %TEMP%\NetConditioner\licenses\WinDivert-LICENSE-<随机id>.txt。

请只填写可信的 HTTPS 更新地址。

## 自测

~~~
NetConditioner.exe --self-test
~~~

自测会写入临时文件、实际加载 WinDivert 并进行网络验证，需要管理员权限。检查项包括：快捷键、配置读写、版本与 SHA256、latest.json 校验、本地回环更新检查与下载、自更新 dry-run、生产过滤器回环排除、上下行延时、进程过滤（TCP/下行/IPv6/UDP）、带宽、突发、丢包、恢复和真实网卡方向验证。

日志写入：

~~~
%TEMP%\NetConditioner-selftest.log
~~~

## 故障排查

| 现象 | 处理 |
| --- | --- |
| 启动失败，错误 2 | 删除 %ProgramFiles%\NetConditioner\runtime 后重试，也可检查 ProgramData / LocalAppData 回退目录 |
| 提示“无法准备 WinDivert 运行时” | 检查运行时目录的磁盘空间与写入权限 |
| 启动失败，错误 5 | 使用管理员身份运行 |
| 错误 577 / 1275 | 检查驱动签名设置与安全软件拦截 |
| 错误 1058 | 检查 WinDivert 驱动服务与安全软件 |
| 提示已有不兼容的 WinDivert 驱动 | 退出其他可能加载不同版本的程序，或重启系统 |
| 热键没反应 | 打开“设置”更换热键 |
| 目标进程没有效果 | 确认进程名与任务管理器一致；新连接可能需要短暂等待 |
| 启动后立即恢复 | 查看自测日志、Windows 事件查看器和安全软件记录 |
| 云更新没有生效 | 确认已填写 HTTPS 更新地址并开启自动更新，查看更新日志 |
| 更新后没有重新打开 | 手工启动程序并检查更新日志 |
| 无法替换 exe | 把程序移到可写目录 |
| 构建找不到 clang-cl.exe / lld-link.exe / llvm-rc.exe / cl.exe / link.exe / rc.exe | 安装完整的 LLVM/Clang 并把 bin 目录加入 PATH，或使用 -Toolchain MSVC；cl.exe、link.exe 与 rc.exe 由 VS Build Tools + Windows SDK 提供 |

## 构建与发布

### 环境要求

- Windows 10 / 11 x64。
- 完整 LLVM/Clang（需要 clang-cl.exe、lld-link.exe、llvm-rc.exe 已加入 PATH；开发验证版本 23.1.6）。
- Visual Studio 2022 Build Tools，包含“使用 C++ 的桌面开发”工作负载（提供 Windows 头文件与库；MSVC 回退时也提供 link.exe/rc.exe）。
- Windows SDK（提供 Windows 库；MSVC 回退时也提供 rc.exe）。
- 构建脚本需要 Windows PowerShell 5.1（也兼容 PowerShell 7）；build.ps1 使用 UTF-8 BOM 保存中文。

源码使用 C17，默认由 clang-cl 编译、lld-link 链接、llvm-rc 编译资源为 x64 单文件 exe；使用 -Toolchain MSVC 时改由 cl.exe/link.exe/rc.exe。不依赖第三方运行库，third_party 只包含 WinDivert。

### 构建

~~~
cd NetConditioner
.\build.ps1
~~~

默认以 Release 配置、完整 LLVM 工具链编译并运行单元测试，然后生成发布产物。构建脚本会自动定位 Visual Studio 2022 的 vcvars64.bat，调用 clang-cl.exe/lld-link.exe/llvm-rc.exe，并检查 exe 的 PE machine 必须为 x64。clang-cl 不支持 /MP，构建脚本会并行启动多个 clang-cl 进程（最多 min(CPU 核数, 12)）编译各个源文件；使用 -Toolchain MSVC 时则走 cl.exe/link.exe/rc.exe，编译阶段使用 /MP。

也可以运行 build.cmd 并附加相同参数。它默认不暂停，适合自动化；双击使用时请显式加 -Pause，以便在窗口关闭前查看结果：

~~~bat
build.cmd -Pause
~~~

只编译和测试（不生成 dist 产物）：

~~~
.\build.ps1 -SkipPackage
~~~

跳过测试：

~~~
.\build.ps1 -SkipTests
~~~

Debug 构建只输出到 dist\debug\NetConditioner-<版本>-win-x64-debug.exe，不会生成或更新 latest.json，不得作为正式版发布：

~~~
.\build.ps1 -Configuration Debug -SkipTests
~~~

单元测试会被编译为 build\tests\NetConditionerTests.exe，由构建脚本自动运行；测试不需要管理员权限，也不会联网或改动真实配置。

回退到 MSVC 编译器：

~~~
.\build.ps1 -Toolchain MSVC
~~~

### 版本号管理

exe 版本来自仓库根目录的 version.txt，格式必须为 2–4 个规范数值段（不允许前导零，每段不超过 Int32）。app.manifest 的 assemblyIdentity.version 必须同步为四位版本（例如 version.txt 的 2.12.0 对应 manifest 的 2.12.0.0）；构建脚本会在编译前检查两者，不一致即失败。修改版本时需同时更新这两个文件并提交。

### 构建参数

| 参数 | 说明 |
| --- | --- |
| -Toolchain <LLVM\|MSVC> | 工具链；默认 LLVM（clang-cl + lld-link + llvm-rc）。MSVC 模式回退到 cl.exe/link.exe/rc.exe，Clang 作为兼容别名 |
| -Version <版本> | 可选；必须与 version.txt 的规范版本完全一致，否则失败。省略时直接读取 version.txt |
| -UpdateBaseUrl <url> | 更新包 URL 前缀 |
| -PageUrl <url> | 清单中的发布页面地址 |
| -Notes <文本> | 清单中的更新说明 |
| -Clean | 构建前清理 build 和 dist |
| -SkipTests | 跳过单元测试 |
| -SkipPackage | 只编译和测试 |
| -Configuration Debug | 使用 Debug 构建并输出到 dist\debug\；默认 Release |
| -Pause | 仅供 build.cmd 使用；进程退出后暂停，默认不暂停 |

### 发布产物与检查

Release 构建生成：

~~~
dist\NetConditioner-<版本>-win-x64.exe
dist\latest.json
~~~

将同一构建的 exe 和 latest.json 上传到同一个 Release；latest.json 中的版本、URL、SHA256 和大小必须对应该 exe。Debug 产物不会生成或更新 latest.json。

当前发布流程不对 exe 进行 Authenticode 签名。未签名程序可能触发 SmartScreen 提示；清单中的 requireAdministrator 只用于请求管理员权限，不代表代码签名。如需签名，应在本次构建产生的 exe 上签名后，用签名后的文件重新计算并写入 latest.json 的 SHA256 和大小，不要先上传旧清单。

### 从精确提交打标签

1. 确认 version.txt、app.manifest 和所有源码改动均已提交，工作区干净。
2. 从该提交执行 .\build.ps1（不要跳过测试），确认 x64 产物、版本和 latest.json 一致。
3. 记录该提交的完整 SHA，并为这个精确提交创建版本标签，例如：

   ~~~powershell
   git rev-parse HEAD
   git tag -a v2.12.0 <上述完整提交SHA> -m "NetConditioner v2.12.0"
   ~~~

4. 推送提交和标签后，从该标签对应的同一构建产物创建 Release。不要在另一个提交上补建同版本标签；若标签已存在且指向错误，必须先由维护者授权并评估对现有用户更新的影响，不能直接移动。

## 源码结构

~~~
src\main.c              入口与命令行分流
src\app.c               应用状态机与主窗口消息
src\win.c               Win32 薄封装
src\util.c              UTF-8/UTF-16、字符串、路径、时间、哈希
src\json.c              JSON DOM 解析与序列化
src\config.c            配置模型、清洗与校验
src\store.c             配置文件读取、原子写入与恢复
src\engine\             弱网引擎（WinDivert、方向队列、进程过滤）
src\ui\                 主窗口、控件、托盘与三个对话框
src\update\             云更新、自更新、更新检查
src\selftest.c          管理员自测
tests\                  单元测试
~~~

## 许可

WinDivert 由 Basil 开发，采用 **LGPLv3 或 GPLv2** 双许可选择。许可全文见 [third_party/WinDivert/LICENSE](third_party/WinDivert/LICENSE)，也可以运行 NetConditioner.exe --licenses 导出。

WinDivert 源码：<https://github.com/basil00/WinDivert>。
