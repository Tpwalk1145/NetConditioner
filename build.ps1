#Requires -Version 5.1
<#
.SYNOPSIS
    NetConditioner 一键构建：cargo 编译 + 测试 + 生成单文件 exe 与云更新清单。

.DESCRIPTION
    默认按顺序执行：
      1. cargo build --release --locked --target x86_64-pc-windows-msvc
      2. cargo test --release --locked --target x86_64-pc-windows-msvc
      3. 把 target\x86_64-pc-windows-msvc\release\NetConditioner.exe 复制为
         dist\NetConditioner-<version>-win-x64.exe，并生成 dist\latest.json

    版本始终取自 Cargo.toml 的 [package].version；-Version 只能原样确认该版本。
    Debug 构建只输出到 dist\debug\，不会生成或更新 latest.json。

    单文件版已内置 WinDivert 运行时，首次启用弱网引擎时会准备到
    %ProgramFiles%\NetConditioner\runtime\<版本>。构建产物为单文件 exe 与 latest.json。
    app.manifest 的 assemblyIdentity.version 必须与 Cargo.toml 同步（不足四位补 0）。

    依赖：Rust（MSVC 工具链，需要 VS2022 Build Tools + Windows SDK）。
    本脚本使用 Windows PowerShell 5.1 兼容语法，文件以 UTF-8 BOM 保存。

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Version 2.1.0 -Clean

.EXAMPLE
    .\build.ps1 -SkipPackage      # 只编译（并跑测试），不生成 dist 产物

.EXAMPLE
    .\build.ps1 -Configuration Debug -SkipTests   # 输出到 dist\debug，无 latest.json
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Version,
    [string]$UpdateBaseUrl = 'https://github.com/Tpwalk1145/NetConditioner/releases/latest/download',
    [string]$PageUrl = 'https://github.com/Tpwalk1145/NetConditioner/releases',
    [string]$Notes,
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$SkipPackage,
    # 供 build.cmd 识别并在进程退出后暂停；直接运行 build.ps1 时不自动暂停。
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$cargoToml = Join-Path $repoRoot 'Cargo.toml'
$appManifest = Join-Path $repoRoot 'app.manifest'
$distDir = Join-Path $repoRoot 'dist'
$targetTriple = 'x86_64-pc-windows-msvc'
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Get-CanonicalVersion {
    param([string]$Value)

    # 2-4 个规范十进制段；不接受前导零、后缀、空白或空段。
    if ($Value -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*))?(\.(0|[1-9][0-9]*))?$') {
        return $null
    }
    foreach ($segment in $Value.Split('.')) {
        $number = 0
        if (-not [int]::TryParse($segment, [System.Globalization.NumberStyles]::None,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
            return $null
        }
    }
    return $Value
}

function Get-CargoVersion {
    param([string]$Path)

    $inPackage = $false
    $version = $null
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match '^\s*\[package\]\s*(?:#.*)?$') {
            $inPackage = $true
            continue
        }
        if ($inPackage -and $line -match '^\s*\[') { break }
        if ($inPackage -and $line -match '^\s*version\s*=\s*"([^"]+)"\s*(?:#.*)?$') {
            $version = $Matches[1]
            break
        }
    }
    if (-not $version) {
        throw "无法从 Cargo.toml 的 [package] 读取版本：$Path"
    }
    return $version
}

function Assert-X64Executable {
    param([string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 64
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length -or
            $header[0] -ne 0x4D -or $header[1] -ne 0x5A) {
            throw "编译产物不是有效的 PE 文件：$Path"
        }
        $peOffset = [System.BitConverter]::ToInt32($header, 0x3C)
        if ($peOffset -lt 0 -or $peOffset + 6 -gt $stream.Length) { throw "PE 头无效：$Path" }
        $stream.Position = $peOffset
        $peHeader = New-Object byte[] 6
        if ($stream.Read($peHeader, 0, $peHeader.Length) -ne $peHeader.Length -or
            $peHeader[0] -ne 0x50 -or $peHeader[1] -ne 0x45 -or
            $peHeader[2] -ne 0 -or $peHeader[3] -ne 0) {
            throw "PE 签名无效：$Path"
        }
        $machine = [System.BitConverter]::ToUInt16($peHeader, 4)
        if ($machine -ne 0x8664) {
            throw "编译产物不是 x64（PE machine 0x{0:X4}）：$Path" -f $machine
        }
    } finally {
        $stream.Dispose()
    }
}

Write-Host '========================================' -ForegroundColor Cyan
Write-Host ' NetConditioner 构建（单文件版）' -ForegroundColor Cyan
Write-Host '========================================' -ForegroundColor Cyan

if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    throw '未找到 cargo，请先安装 Rust 工具链：https://rustup.rs'
}
if (-not (Test-Path -LiteralPath $cargoToml)) {
    throw "未找到项目文件：$cargoToml"
}

$cargoVersion = Get-CargoVersion -Path $cargoToml
$canonicalVersion = Get-CanonicalVersion -Value $cargoVersion
if (-not $canonicalVersion) {
    throw "Cargo.toml 版本格式无效：$cargoVersion（要求 2-4 个数值段，每段不超过 Int32）"
}
if ($PSBoundParameters.ContainsKey('Version')) {
    if (-not $Version) { throw '-Version 不能为空；省略该参数即使用 Cargo.toml 版本。' }
    if ((Get-CanonicalVersion -Value $Version) -cne $canonicalVersion) {
        throw "-Version 必须与 Cargo.toml 的规范版本完全一致：$canonicalVersion（实际：$Version）"
    }
}
$Version = $canonicalVersion

# manifest 由 rc.exe 嵌入 exe；版本不一致时在编译前失败，避免发布旧版本清单。
if (-not (Test-Path -LiteralPath $appManifest)) {
    throw "未找到应用程序清单：$appManifest"
}
[xml]$manifestXml = Get-Content -LiteralPath $appManifest -Raw
$identity = $manifestXml.SelectSingleNode('/*[local-name()="assembly"]/*[local-name()="assemblyIdentity"]')
if (-not $identity) { throw "app.manifest 缺少 assemblyIdentity：$appManifest" }
$versionParts = @($Version.Split('.') + @('0', '0', '0', '0'))[0..3]
$expectedManifestVersion = $versionParts -join '.'
if ($identity.GetAttribute('version') -cne $expectedManifestVersion) {
    throw "app.manifest assemblyIdentity.version 必须为 $expectedManifestVersion（与 Cargo.toml $Version 同步）"
}

Write-Host "  cargo    : $(cargo --version)"
Write-Host "  rustc    : $(rustc --version)"
Write-Host "  配置     : $Configuration"
Write-Host "  目标     : $targetTriple"
Write-Host "  版本     : $Version（Cargo.toml）"
Write-Host ''

if ($Clean) {
    Write-Host '清理 target / dist ...' -ForegroundColor DarkGray
    foreach ($name in @('target', 'dist')) {
        $path = Join-Path $repoRoot $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        } catch {
            Write-Warning "无法完全删除 $path（可能仍有 NetConditioner 在运行）：$($_.Exception.Message)"
        }
    }
}

Write-Host '[1/3] 编译应用程序...' -ForegroundColor Cyan
$cargoBuildArgs = @('build', '--locked', '--target', $targetTriple)
if ($Configuration -eq 'Release') { $cargoBuildArgs += '--release' }
& cargo @cargoBuildArgs
if ($LASTEXITCODE -ne 0) {
    throw "cargo build 失败（退出码 $LASTEXITCODE）"
}

if (-not $SkipTests) {
    Write-Host ''
    Write-Host '[2/3] 运行单元测试...' -ForegroundColor Cyan
    $cargoTestArgs = @('test', '--locked', '--target', $targetTriple)
    if ($Configuration -eq 'Release') { $cargoTestArgs += '--release' }
    & cargo @cargoTestArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cargo test 失败（退出码 $LASTEXITCODE）"
    }
} else {
    Write-Host '[2/3] 已跳过测试（-SkipTests）。' -ForegroundColor DarkGray
}

$profileDir = if ($Configuration -eq 'Release') { 'release' } else { 'debug' }
$targetDir = Join-Path (Join-Path (Join-Path $repoRoot 'target') $targetTriple) $profileDir
$executable = Join-Path $targetDir 'NetConditioner.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    throw "未找到编译产物：$executable"
}
Assert-X64Executable -Path $executable

if ($SkipPackage) {
    $stopwatch.Stop()
    Write-Host ''
    Write-Host "构建完成（已跳过打包），耗时 $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒。" -ForegroundColor Green
    Write-Host "输出：$executable"
    return
}

if ($Configuration -eq 'Release') {
    if (-not $UpdateBaseUrl) {
        throw '未提供 -UpdateBaseUrl，无法生成 latest.json 中的下载地址。'
    }
    $baseUrl = $UpdateBaseUrl.TrimEnd('/')
    if (-not $PageUrl) { $PageUrl = $baseUrl }
    if (-not $Notes) { $Notes = "版本 $Version" }
}

Write-Host ''
Write-Host '[3/3] 生成单文件产物...' -ForegroundColor Cyan
$artifactName = "NetConditioner-$Version-win-x64.exe"
if ($Configuration -eq 'Debug') {
    $packageDir = Join-Path $distDir 'debug'
    $artifactName = "NetConditioner-$Version-win-x64-debug.exe"
} else {
    $packageDir = $distDir
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
$artifactPath = Join-Path $packageDir $artifactName
Copy-Item -LiteralPath $executable -Destination $artifactPath -Force

$artifact = Get-Item -LiteralPath $artifactPath
$hash = Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256
$manifestPath = $null

if ($Configuration -eq 'Release') {
    $manifest = [ordered]@{
        schemaVersion = 2
        version       = $Version
        publishedAt   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        notes         = $Notes
        pageUrl       = $PageUrl.TrimEnd('/')
        installer     = [ordered]@{
            url    = "$baseUrl/$artifactName"
            sha256 = $hash.Hash
            size   = $artifact.Length
        }
    }

    $manifestPath = Join-Path $distDir 'latest.json'
    $manifestJson = ($manifest | ConvertTo-Json -Depth 5) + [Environment]::NewLine
    [System.IO.File]::WriteAllText(
        $manifestPath, $manifestJson, (New-Object System.Text.UTF8Encoding($false)))
}

$stopwatch.Stop()
Write-Host ''
Write-Host '========================================' -ForegroundColor Green
Write-Host ' 构建完成' -ForegroundColor Green
Write-Host '========================================' -ForegroundColor Green
Write-Host "  耗时   : $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒"
Write-Host "  单文件 : $($artifact.FullName)"
Write-Host "  大小   : $([math]::Round($artifact.Length / 1MB, 2)) MB"
Write-Host "  SHA256 : $($hash.Hash)"
if ($manifestPath) {
    Write-Host "  清单   : $manifestPath"
    Write-Host '  分发时只需要上传这个 exe 和 latest.json。'
} else {
    Write-Warning '这是 Debug 产物，仅保存在 dist\debug\；不会生成或更新 latest.json，请勿作为正式版发布。'
}
