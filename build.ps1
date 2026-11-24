#Requires -Version 5.1
<#
.SYNOPSIS
    NetConditioner 一键构建：cargo 编译 + 测试 + 生成单文件 exe（可选生成云更新清单）。

.DESCRIPTION
    默认按顺序执行：
      1. cargo build（Release）
      2. cargo test（可用 -SkipTests 跳过）
      3. 把 target\release\NetConditioner.exe 复制为
         dist\NetConditioner-<version>-win-x64.exe，并生成 dist\latest.json

    单文件版已内置 WinDivert 运行时，首次运行会自动解压到
    %ProgramFiles%\NetConditioner\runtime\<版本>\，不再需要安装包。

    依赖：Rust（MSVC 工具链，需要 VS2022 Build Tools + Windows SDK）。

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Version 2.1.1 -Clean

.EXAMPLE
    .\build.ps1 -SkipPackage      # 只编译（并跑测试），不生成 dist 产物
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Version,
    [string]$UpdateBaseUrl = 'https://github.com/Tpwalk1145/NetConditioner/releases/latest/download',
    [string]$PageUrl = 'https://github.com/Tpwalk1145/NetConditioner/releases',
    [string]$Notes,
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$cargoToml = Join-Path $repoRoot 'Cargo.toml'
$distDir = Join-Path $repoRoot 'dist'
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Get-CargoVersion {
    param([string]$Path)
    $match = Select-String -LiteralPath $Path -Pattern '^\s*version\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $match) { return '0.0.0' }
    return $match.Matches[0].Groups[1].Value
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
if (-not $Version) {
    $Version = $cargoVersion
} elseif ($Version -ne $cargoVersion) {
    Write-Warning "Cargo.toml 中的版本是 $cargoVersion；exe 内嵌版本不会随 -Version 改变，只有文件名/latest.json 使用 $Version。"
}

Write-Host "  cargo    : $(cargo --version)"
Write-Host "  rustc    : $(rustc --version)"
Write-Host "  配置     : $Configuration"
Write-Host "  版本     : $Version（Cargo.toml: $cargoVersion）"
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
$cargoBuildArgs = @('build')
if ($Configuration -eq 'Release') { $cargoBuildArgs += '--release' }
& cargo @cargoBuildArgs
if ($LASTEXITCODE -ne 0) {
    throw "cargo build 失败（退出码 $LASTEXITCODE）"
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

$targetDir = Join-Path $repoRoot (Join-Path 'target' $(if ($Configuration -eq 'Release') { 'release' } else { 'debug' }))
$executable = Join-Path $targetDir 'NetConditioner.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    throw "未找到编译产物：$executable"
}

if ($SkipPackage) {
    $stopwatch.Stop()
    Write-Host ''
    Write-Host "构建完成（已跳过打包），耗时 $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒。" -ForegroundColor Green
    Write-Host "输出：$executable"
    return
}

Write-Host ''
Write-Host '[3/3] 生成单文件产物与更新清单...' -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$artifactName = "NetConditioner-$Version-win-x64.exe"
$artifactPath = Join-Path $distDir $artifactName
Copy-Item -LiteralPath $executable -Destination $artifactPath -Force

$artifact = Get-Item -LiteralPath $artifactPath
$hash = Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256

if (-not $UpdateBaseUrl) {
    throw '未提供 -UpdateBaseUrl，无法生成 latest.json 中的下载地址。'
}
$baseUrl = $UpdateBaseUrl.TrimEnd('/')
if (-not $PageUrl) { $PageUrl = $baseUrl }
if (-not $Notes) { $Notes = "版本 $Version" }

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

$stopwatch.Stop()
Write-Host ''
Write-Host '========================================' -ForegroundColor Green
Write-Host ' 构建完成' -ForegroundColor Green
Write-Host '========================================' -ForegroundColor Green
Write-Host "  耗时   : $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒"
Write-Host "  单文件 : $($artifact.FullName)"
Write-Host "  大小   : $([math]::Round($artifact.Length / 1MB, 2)) MB"
Write-Host "  SHA256 : $($hash.Hash)"
Write-Host "  清单   : $manifestPath"
Write-Host '  分发时只需要上传这个 exe 和 latest.json。'

