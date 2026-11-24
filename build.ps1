#Requires -Version 5.1
<#
.SYNOPSIS
    NetConditioner 一键构建：cargo 编译 -> 发布 -> Inno Setup 安装包。

.DESCRIPTION
    根目录构建入口，默认按顺序执行：
      1. cargo build（Release）
      2. cargo test（可用 -SkipTests 跳过）
      3. installer\build-installer.ps1：发布单文件 exe 并调用 ISCC.exe 打包

    产物：dist\NetConditioner-<version>-win-x64-Setup.exe 与 dist\latest.json

    依赖：Rust（MSVC 工具链，需要 VS2022 Build Tools + Windows SDK）、Inno Setup 6。

.PARAMETER Configuration
    Release（默认）或 Debug。

.PARAMETER Version
    安装包/manifest 版本号；默认从 Cargo.toml 读取。exe 内嵌版本始终是 Cargo.toml 的值。

.PARAMETER Clean
    先删除 target / publish / dist 再构建。

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Version 2.0.1 -Clean

.EXAMPLE
    .\build.ps1 -SkipInstaller     # 只编译（并跑测试），不发布、不打包
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Version,
    [string]$ISCCPath,
    [string]$UpdateBaseUrl,
    [string]$Notes,
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$SkipPublish,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$cargoToml = Join-Path $repoRoot 'Cargo.toml'
$installerScript = Join-Path $repoRoot 'installer\build-installer.ps1'
$distDir = Join-Path $repoRoot 'dist'
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Get-CargoVersion {
    param([string]$Path)
    $match = Select-String -LiteralPath $Path -Pattern '^\s*version\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $match) { return '0.0.0' }
    return $match.Matches[0].Groups[1].Value
}

function Test-InnoSetupAvailable {
    if ($ISCCPath) { return (Test-Path -LiteralPath $ISCCPath) }
    if (Get-Command ISCC.exe -ErrorAction SilentlyContinue) { return $true }

    $candidates = @()
    if ($env:INNO_SETUP_HOME) { $candidates += (Join-Path $env:INNO_SETUP_HOME 'ISCC.exe') }
    $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    foreach ($programFiles in @([Environment]::GetFolderPath('ProgramFilesX86'), [Environment]::GetFolderPath('ProgramFiles'))) {
        if ($programFiles) { $candidates += (Join-Path $programFiles 'Inno Setup 6\ISCC.exe') }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $true }
    }
    return $false
}

Write-Host '========================================' -ForegroundColor Cyan
Write-Host ' NetConditioner 构建' -ForegroundColor Cyan
Write-Host '========================================' -ForegroundColor Cyan

if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    throw '未找到 cargo，请先安装 Rust 工具链：https://rustup.rs'
}
if (-not (Test-Path -LiteralPath $cargoToml)) {
    throw "未找到项目文件：$cargoToml"
}
if (-not $SkipInstaller -and -not (Test-Path -LiteralPath $installerScript)) {
    throw "未找到打包脚本：$installerScript"
}
if (-not $SkipInstaller -and -not (Test-InnoSetupAvailable)) {
    throw '未找到 Inno Setup 6（ISCC.exe）。请先安装：winget install JRSoftware.InnoSetup，或用 -ISCCPath 指定路径。'
}

$cargoVersion = Get-CargoVersion -Path $cargoToml
if (-not $Version) {
    $Version = $cargoVersion
} elseif ($Version -ne $cargoVersion) {
    Write-Warning "Cargo.toml 中的版本是 $cargoVersion；exe 内嵌版本不会随 -Version 改变，只有安装包/latest.json 使用 $Version。"
}

Write-Host "  cargo    : $(cargo --version)"
Write-Host "  rustc    : $(rustc --version)"
Write-Host "  配置     : $Configuration"
Write-Host "  版本     : $Version（Cargo.toml: $cargoVersion）"
Write-Host ''

if ($Clean) {
    Write-Host '清理 target / publish / dist ...' -ForegroundColor DarkGray
    foreach ($name in @('target', 'publish', 'dist')) {
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
$cargoBuildArgs = @('build')
if ($Configuration -eq 'Release') { $cargoBuildArgs += '--release' }
& cargo @cargoBuildArgs
if ($LASTEXITCODE -ne 0) {
    throw "cargo build 失败（退出码 $LASTEXITCODE）"
}

# 开发便利：把 WinDivert 文件放到 exe 同目录，便于直接从 target 运行。
$outputDir = Join-Path $repoRoot (Join-Path 'target' $(if ($Configuration -eq 'Release') { 'release' } else { 'debug' }))
$thirdParty = Join-Path $repoRoot 'third_party\WinDivert'
foreach ($name in @('WinDivert.dll', 'WinDivert64.sys')) {
    Copy-Item -LiteralPath (Join-Path $thirdParty $name) -Destination (Join-Path $outputDir $name) -Force -ErrorAction SilentlyContinue
}

if (-not $SkipTests) {
    Write-Host ''
    Write-Host '[2/3] 运行单元测试...' -ForegroundColor Cyan
    $cargoTestArgs = @('test')
    if ($Configuration -eq 'Release') { $cargoTestArgs += '--release' }
    & cargo @cargoTestArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cargo test 失败（退出码 $LASTEXITCODE）"
    }
} else {
    Write-Host '[2/3] 已跳过测试（-SkipTests）。' -ForegroundColor DarkGray
}

if ($SkipInstaller) {
    $stopwatch.Stop()
    Write-Host ''
    Write-Host "构建完成（已跳过安装包），耗时 $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒。" -ForegroundColor Green
    Write-Host "输出：$outputDir"
    return
}

Write-Host ''
Write-Host '[3/3] 发布并打包安装包...' -ForegroundColor Cyan

$installerArgs = @{
    Configuration = $Configuration
    Version       = $Version
}
if ($ISCCPath) { $installerArgs['ISCCPath'] = $ISCCPath }
if ($UpdateBaseUrl) { $installerArgs['UpdateBaseUrl'] = $UpdateBaseUrl }
if ($Notes) { $installerArgs['Notes'] = $Notes }
if ($SkipPublish) { $installerArgs['SkipPublish'] = $true }

& $installerScript @installerArgs
if ($LASTEXITCODE -ne 0) {
    throw "打包脚本失败（退出码 $LASTEXITCODE）"
}

$stopwatch.Stop()
$setupPath = Join-Path $distDir "NetConditioner-$Version-win-x64-Setup.exe"
if (-not (Test-Path -LiteralPath $setupPath)) {
    $setupPath = Get-ChildItem -LiteralPath $distDir -Filter '*Setup.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

Write-Host ''
Write-Host '========================================' -ForegroundColor Green
Write-Host ' 构建完成' -ForegroundColor Green
Write-Host '========================================' -ForegroundColor Green
Write-Host "  耗时   : $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒"
if ($setupPath -and (Test-Path -LiteralPath $setupPath)) {
    $setup = Get-Item -LiteralPath $setupPath
    Write-Host "  安装包 : $($setup.FullName)"
    Write-Host "  大小   : $([math]::Round($setup.Length / 1MB, 2)) MB"
    Write-Host "  清单   : $(Join-Path $distDir 'latest.json')"
}

