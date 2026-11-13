# NetConditioner 弱网模拟器

Windows 系统级弱网模拟工具：通过全局热键一键开启/停止上行、下行的延时、抖动、带宽、随机丢包和连续丢包。界面参考弱网测试 App 的配置方式，做成“VPN 客户端”的连接/断开形式，但底层使用 **WinDivert 2.2.2** 在网络层拦截并重注入 TCP/UDP，不需要 TUN/TAP 虚拟网卡，也不会修改系统路由、DNS 或防火墙。

## 功能

- 上行 / 下行独立配置：
  - 延时（ms）
  - 延时抖动（ms）
  - 带宽（kbps，0 = 不限速）
  - 随机丢包（0–100%）
  - 连续丢包：放行时长 + 丢包时长循环（ms）
- 弱网协议：`TCP`、`UDP`、`ALL`（TCP + UDP）
- 目标进程过滤（全局设置）：主窗口“启动弱网”上方可选择程序，只对它们生效；**不选任何进程 = 全局**
  - 按进程名匹配（如 `chrome.exe`），同一程序的多开实例都会生效
  - 支持按进程名搜索；勾选即生效
- 多套配置：新建、编辑、删除、快速切换
- 小窗口 + 系统托盘 + 自定义全局热键（默认 `Ctrl + Alt + U`；也支持单键，如 `F8`）
  - 单键会全局占用该按键，例如把字母 `A` 设为热键后，正常打字也会触发；建议优先用 F1–F12 等不常用键。
- 实时统计：上下行发送、丢弃、队列长度
- 云更新：配置更新地址后，后台静默检查新版本并自动升级（不弹窗，升级时窗口闪一下重启）
- 最长运行时间保护：到时自动停止并恢复网络
- 最大额外排队时延保护：避免突发流量把队列堆到几分钟，造成严重卡顿
- 配置与目标进程设置保存在 `%AppData%\NetConditioner\config.json`

## 一个重要限制：TCP 下载依赖上行 ACK

“上行完全为 0、下载不受影响”在 TCP 协议上不可能实现：客户端必须持续向上游发送 ACK，TLS/HTTP2/QUIC 还需要握手和控制包。因此：

- 上行带宽设得很低时，下载也会变慢；
- 上行随机丢包设为 100% 时，网页、下载、远程桌面都会中断；
- 本工具是网络条件模拟器，不会绕过这个协议事实。

如果目标是“限制大流量上传但尽量保留下载”，建议上行带宽从 32–128 kbps 开始试，而不是直接 0/100% 丢包。

## 环境要求

- Windows 10 / 11 x64
- 管理员权限（WinDivert 驱动需要）
- 运行程序：**不需要 .NET 运行时**（程序以 NativeAOT 编译成原生单文件 exe）
- 构建程序：需要 .NET 11 SDK；打包安装包另需 Inno Setup 6
- 随程序附带的 `WinDivert.dll` / `WinDivert64.sys`（2.2.2 官方版本）

## 构建与运行

一键构建（编译 + 发布 + 打包安装包）：

```powershell
cd NetConditioner
.\build.ps1
```

也可以直接双击根目录的 `build.cmd`（等价于执行 `build.ps1`）。

只编译、不打包：

```powershell
.\build.ps1 -SkipInstaller
```

或者手动执行：

```powershell
cd NetConditioner
dotnet build -c Release
```

发布 NativeAOT 单文件（原生 exe，无需 .NET 运行时）：

```powershell
dotnet publish -c Release -r win-x64 -p:PublishProfile=Aot -o publish\win-x64-selfcontained
```

运行（AOT 发布目录）：

```text
NetConditioner\publish\win-x64-selfcontained\NetConditioner.exe
```

运行只需要下面这三个文件（约 21 MB；发布目录里的 `third_party` 仅为 WinDivert 许可文件）：

```text
NetConditioner.exe
WinDivert.dll
WinDivert64.sys
```

`bin\Release\net11.0-windows\NetConditioner.exe` 是普通托管构建，仅供本机调试，运行时需要 .NET 11 Desktop Runtime。

首次运行会弹出 UAC；程序需要以管理员身份运行。`WinDivert.dll` 会自动加载同目录的 `WinDivert64.sys`。

## 打包安装包

使用 [Inno Setup 6](https://jrsoftware.org/isinfo.php) 制作安装包。程序以 NativeAOT 编译成单文件原生 exe，目标机器**无需安装 .NET 运行时**。

1. 安装 Inno Setup 6：

   ```powershell
   winget install JRSoftware.InnoSetup
   ```

2. 在仓库根目录执行一键构建脚本（编译 → 发布 → 打包）：

   ```powershell
   .\build.ps1
   ```

   也可以直接双击根目录的 `build.cmd`。脚本会先编译 Release，再以 `win-x64`、NativeAOT 单文件方式发布程序，最后调用 `ISCC.exe` 生成：

   ```text
   dist\NetConditioner-1.0.0-win-x64-Setup.exe
   ```

3. 双击安装包安装；会创建开始菜单快捷方式和卸载项，默认安装到 `C:\Program Files\NetConditioner`。程序仍按 `app.manifest` 请求管理员权限。

`build.ps1` 常用参数：

| 参数 | 说明 |
| --- | --- |
| `-Version 1.1.0` | 指定安装包版本（默认取 csproj 里的 `Version`） |
| `-UpdateBaseUrl <url>` | 云更新地址前缀；生成 `dist\latest.json` 时用作安装包 URL（默认占位地址，上传前需替换） |
| `-Notes <文本>` | 写进 `latest.json` 的更新说明 |
| `-Clean` | 先删除 `bin` / `obj` / `publish` / `dist` 再构建 |
| `-SkipInstaller` | 只编译，不发布、不打包 |
| `-SkipPublish` | 复用现有 `publish` 目录，只重新生成安装包 |
| `-Configuration Debug` | 指定构建配置（默认 `Release`） |
| `-ISCCPath <路径>` | 手动指定 `ISCC.exe` |

说明：

- 安装包约 7 MB：NativeAOT 单文件约 21 MB，再经 LZMA2 压缩（旧的自包含发布约 38 MB）。
- `ISCC.exe` 不在 PATH 时，可用 `-ISCCPath` 指定，或设置 `INNO_SETUP_HOME` 环境变量；未安装 Inno Setup 时脚本会直接报错提示。
- 根目录 `build.ps1` 是入口，实际的发布 + ISCC 编译逻辑在 `installer\build-installer.ps1`，也可以单独调用它。
- 构建完成后会同时生成 `dist\latest.json`（云更新清单），把它和安装包一起上传即可。
- 配置仍保存在 `%AppData%\NetConditioner\config.json`，卸载不会删除。

### NativeAOT 说明

- 发布配置在 `Properties/PublishProfiles/Aot.pubxml`，用 `dotnet publish -p:PublishProfile=Aot` 调用。
- **不要**在 `NetConditioner.csproj` 里全局设置 `PublishAot`：那会让 `dotnet build` 因 Windows 窗体 + 剪裁检查（NETSDK1175）直接失败。
- 为 AOT 做的适配：
  - `config.json` 改用 System.Text.Json 源生成（`Models/ConfigJsonContext.cs`），不再依赖反射序列化；旧配置文件可以直接读取，输出格式不变；
  - 配置下拉框不再使用 `DisplayMember` 数据绑定（裁剪后 WinForms 会抛 `NotSupportedException`），改为依赖 `NetworkProfile.ToString()`。
- Windows 窗体官方未正式支持 NativeAOT，发布时会出现 `IL3053` / `IL3000` 分析警告（来自 WinForms 程序集自身），属已知情况。当前产物已通过 `NetConditioner.exe --self-test` 全项自测，并冒烟验证了主窗口、配置编辑、设置、进程选择、托盘隐藏/退出；启动约 0.2 秒。
- 升级 .NET SDK 后请重新跑自测并冒烟上述窗口。

## 云更新（静默自动升级）

配置更新地址后，程序会在后台**完全静默**地检查、下载、校验并安装新版本：没有弹窗、没有托盘气泡、没有安装向导，唯一可见的现象是升级时窗口闪一下后重新出现。

### 行为

- 启动约 5 秒后检查一次，之后每 6 小时检查一次；**每天最多检查一次**（距上次检查不足 24 小时则跳过）。
- 发现新版本后自动下载到 `%TEMP%\NetConditioner\update\`，边下边算 SHA256；校验失败会删除文件，并在 24 小时内不再尝试同一版本。
- 只有“空闲”时才安装：弱网未运行、没有正在切换、没有打开设置/编辑等对话框。弱网运行中只等待，不打断；停止弱网或隐藏到托盘后会继续静默安装。
- 安装时用 `/VERYSILENT` 静默运行安装包，安装完成后自动重新启动程序。
- 所有失败只写入 `%TEMP%\NetConditioner\update\update.log`，不打扰用户。

### 配置

打开“设置 → 软件更新”：

| 项 | 说明 |
| --- | --- |
| 更新地址（latest.json） | 例如 `https://你的域名/NetConditioner/latest.json`；留空 = 关闭云更新（默认） |
| 自动静默更新 | 默认开启；关闭后不会检查更新 |

仅支持 https；本机回环地址（`http://127.0.0.1:...`）允许 http，便于本地联调。

### 生成更新清单

```powershell
.\build.ps1 -UpdateBaseUrl https://你的域名/NetConditioner -Notes "修复若干问题"
```

会在 `dist\` 生成 `latest.json`，内容形如：

```json
{
  "schemaVersion": 1,
  "version": "1.1.0",
  "publishedAt": "2026-11-13T00:00:00Z",
  "notes": "修复若干问题",
  "pageUrl": "https://你的域名/NetConditioner/",
  "installer": {
    "url": "https://你的域名/NetConditioner/NetConditioner-1.1.0-win-x64-Setup.exe",
    "sha256": "D9C7F19E...",
    "size": 7123456
  }
}
```

把 `latest.json` 和对应的安装包上传到同一目录即可。任意静态托管（对象存储、Nginx、GitHub Releases）都可以；用 GitHub Releases 时可以用固定地址 `https://github.com/<用户>/<仓库>/releases/latest/download/latest.json`，把 `latest.json` 作为 release 资产上传。

### 诊断

```powershell
# 按当前配置检查一次，结果写到 %TEMP%\NetConditioner-update-check.log（不弹任何界面）
NetConditioner.exe --check-update

# 开发者联调：只记录安装命令，不真正运行安装包
$env:NC_UPDATE_DRY_RUN = "1"
```

### 安全边界

自动更新没有用户确认环节，因此依赖：**https 传输 + latest.json 中的 SHA256 完整性校验 + 更新源可信**。请只把更新地址指向自己控制的服务器；项目未做代码签名。

## 使用

1. 点击“新建”，填写配置名称。
2. 设置上行/下行参数，选择协议（`ALL` 表示 TCP + UDP）。
3. 如需只对指定程序生效，在主窗口“目标进程”处点击“选择进程…”；留空则对所有程序生效。
4. 点击“启动弱网”。
5. 按全局热键或点击“停止并恢复”即可随时停止。
6. 关闭窗口会隐藏到托盘，弱网状态保持不变；从托盘菜单“退出”会停止并恢复网络。

### 参数语义

| 参数 | 说明 |
| --- | --- |
| 延时 | 数据包额外等待的时间 |
| 延时抖动 | 在 `延时 + random(0..抖动)` 范围内随机 |
| 带宽 | 0 = 不限速；按 kbps 串行化发送，从第一个包开始生效 |
| 随机丢包 | 每个命中协议的包按百分比独立丢弃 |
| 连续丢包-放行 | 连续丢包循环中的放行阶段时长 |
| 连续丢包-丢包 | 连续丢包循环中的丢包阶段时长 |

### 目标进程过滤

- 目标进程是主窗口“启动弱网”上方的全局设置，不属于某一套配置；切换配置不会改变目标进程。
- 最多可同时选择 64 个进程。
- **不选任何进程 = 全局**：全系统 TCP/UDP 都应用弱网参数。
- 选择进程后：引擎通过 Windows 的 TCP/UDP 表把本机连接映射到进程，只对命中进程名的流量应用延时/丢包/限速，其它程序的流量直接放行。
- 进程设置保存的是文件名（如 `chrome.exe`），不是 PID；程序重启、多开或稍后启动都能继续匹配。
- 映射每 2 秒刷新一次；新连接会按需刷新。连接刚建立的前几十毫秒可能还没被识别而直通，这是为了不影响其它程序。
- 只覆盖常规 IPv4/IPv6 的 TCP/UDP 包；带不常见 IPv6 扩展头或非首片分片的包会直通。
- 进程过滤模式每个包多一次本地端点查找，开销很小；极高流量下会比全局模式略高一点 CPU。
- 同一文件名的不同程序会被一起匹配；本工具按文件名匹配，不按完整路径，无法区分同名程序。

### 突发流量与卡顿

- 引擎使用 `ArrayPool` 复用包缓冲区，发送线程按批处理，避免每个包分配非托管内存和频繁唤醒。
- WinDivert 内核队列为 16384 个包 / 32MB，给用户态处理留出缓冲。
- “设置”里的“最大额外排队时延”默认 2000ms：只限制超出配置延时之外的排队。超过上限的包会被丢弃（计入“丢弃”），防止低带宽 + 大突发把队列堆到几分钟，导致系统看起来卡死。重启弱网后生效。
- 设为 0 表示不限制排队：突发时更不容易丢包，但可能出现明显延迟甚至长时间卡顿。极端突发下，任何限速器都必须在“丢包”和“高延迟”之间做取舍。
- 64KB / 1MB 突发（10ms 延时、不限速）均无额外丢包，正常的短时突发不会因为排队策略被误丢。

连续丢包规则：

- 两个值都为 0：关闭；
- 只填“丢包”：一直丢包；
- 只填“放行”：不丢包；
- 两个都填：放行 → 丢包 → 放行循环。

### 配置示例

**只限制上行带宽（最接近“上行带宽开关”）**

```text
上行带宽：64 kbps
其余全部为 0
协议：ALL
```

**模拟高延迟链路**

```text
上行延时：300 ms，抖动：100 ms
下行延时：300 ms，抖动：100 ms
```

**模拟上行丢包**

```text
上行随机丢包：50%
```

**完全阻断上行（注意：下载也会停）**

```text
上行随机丢包：100%
```

## 安全与恢复

- 弱网只在程序运行期间生效。
- 停止、退出程序或程序崩溃时，WinDivert 句柄会被关闭，网络立即恢复正常。
- 不修改系统路由、DNS、代理、防火墙或网卡配置。
- WinDivert 驱动会由 DLL 自动安装/加载；驱动服务本身会保留在系统中，这是 WinDivert 的正常行为。
- 如果远程桌面中设置了高丢包/低带宽，可能断开连接。请先在“设置”里设置“最长运行时间”，让程序到时自动恢复。

## 故障排查

| 现象 | 处理 |
| --- | --- |
| 启动失败，错误 2 | `WinDivert.dll` / `WinDivert64.sys` 不在 exe 同目录 |
| 启动失败，错误 5 | 没有以管理员身份运行 |
| 启动失败，错误 577 / 1275 | 驱动签名被拦截或安全软件阻止；检查杀毒软件，使用随包的 2.2.2 官方文件 |
| 提示已有不兼容的 WinDivert 驱动 | 本机其它程序可能加载了不同版本的 WinDivert；退出该程序或重启后再试 |
| 热键没反应 | 热键被其它程序占用，打开“设置”换一个组合 |
| 选了目标进程但没效果 | 确认进程名与任务管理器一致；新启动的程序最多约 2 秒后生效；系统驱动级流量可能没有 TCP/UDP 记录 |
| 启动后立即恢复 | 检查 Windows 事件查看器/杀毒软件；可运行 `NetConditioner.exe --self-test` 查看日志 |
| 云更新没生效 | 看 `%TEMP%\NetConditioner\update\update.log`；确认“设置 → 软件更新”里已填 https 地址并勾选自动静默更新 |
| 更新后程序没重新打开 | 手工启动程序；安装日志在 `%TEMP%\NetConditioner\update\install-*.log` |

自测：

```powershell
NetConditioner.exe --self-test
```

覆盖上下行延时、64kbps 限速、100% 丢包、停止恢复、进程过滤（IPv4/IPv6 TCP、UDP；命中生效 / 未命中直通）、64KB/1MB 突发无额外丢包、真实网卡 80 端口的上下行方向验证，以及云更新（版本比较、latest.json 校验、配置清洗、本地回环 HTTP 下载与 SHA256 校验/篡改拒绝）。结果写入 `%TEMP%\NetConditioner-selftest.log`。自测只影响本机回环测试端口和公网 80 端口，不会影响 443 连接。NativeAOT 发布版同样支持 `--self-test`。

## 与 TUN/TAP、VPN 的关系

本工具不创建 TUN/TAP 虚拟网卡，界面按 VPN 客户端风格设计。Windows 上做系统级弱网模拟的标准方式就是 WinDivert（与 Clumsy 同类）；TUN/TAP 只负责把流量导入用户态，真正做延时/丢包/限速同样需要包处理引擎，而且自建路由/NAT 更容易把本机网络弄断。

## 目录结构

```text
NetConditioner/
  NetConditioner.csproj
  app.manifest                 # requireAdministrator
  build.ps1 / build.cmd        # 一键构建（编译 + AOT 发布 + 打包安装包）
  Program.cs                   # 入口、单实例、--self-test、--check-update
  Models/                      # 配置模型、更新清单模型、System.Text.Json 源生成上下文
  Services/                    # WinDivert、弱网引擎、进程过滤、配置存储、热键、云更新
  Forms/                       # 主窗口、配置编辑、设置、进程选择
  Properties/PublishProfiles/  # NativeAOT 发布配置（Aot.pubxml）
  installer/                   # Inno Setup 脚本、中文语言文件、打包脚本
  third_party/WinDivert/       # WinDivert 2.2.2 DLL/SYS、LICENSE、VERSION
  README.md
```

## 许可

WinDivert 由 Basil（reqrypt.org）开发，采用 LGPLv3 / GPLv2 双许可。本项目动态链接未修改的 WinDivert DLL/驱动，许可全文见 `third_party/WinDivert/LICENSE`。WinDivert 源码：<https://github.com/basil00/WinDivert>。
