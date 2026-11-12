#Requires -Version 5.1
<#
.SYNOPSIS
    NetConditioner 一键构建：编译 Release -> 自包含发布 -> Inno Setup 安装包。

.DESCRIPTION
    根目录构建入口，默认按顺序执行：
      1. dotnet build -c Release 编译检查
      2. dotnet publish win-x64（NativeAOT 单文件，无需 .NET 运行时；
         由 installer\build-installer.ps1 完成）
      3. 调用 ISCC.exe 编译安装包
    产物：dist\NetConditioner-<version>-win-x64-Setup.exe

    依赖：.NET 11 SDK、Inno Setup 6（winget install JRSoftware.InnoSetup）。

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Version 1.0.1 -Clean

.EXAMPLE
    .\build.ps1 -SkipInstaller     # 只编译，不发布、不打包

.EXAMPLE
    .\build.ps1 -SkipPublish       # 复用现有 publish 目录，只重新生成安装包
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Runtime = 'win-x64',
    [string]$Version,
    [string]$ISCCPath,
    [switch]$Clean,
    [switch]$SkipPublish,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$projectPath = Join-Path $repoRoot 'NetConditioner.csproj'
$installerScript = Join-Path $repoRoot 'installer\build-installer.ps1'
$distDir = Join-Path $repoRoot 'dist'
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

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

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw '未找到 dotnet，请先安装 .NET 11 SDK：https://dotnet.microsoft.com/download'
}
if (-not (Test-Path -LiteralPath $projectPath)) {
    throw "未找到项目文件：$projectPath"
}
if (-not $SkipInstaller -and -not (Test-Path -LiteralPath $installerScript)) {
    throw "未找到打包脚本：$installerScript"
}
if (-not $SkipInstaller -and -not (Test-InnoSetupAvailable)) {
    throw '未找到 Inno Setup 6（ISCC.exe）。请先安装：winget install JRSoftware.InnoSetup，或用 -ISCCPath 指定路径。'
}

Write-Host "  .NET SDK : $(dotnet --version)"
Write-Host "  配置     : $Configuration"
Write-Host "  运行时   : $Runtime"
Write-Host '  发布方式 : NativeAOT 单文件（目标机无需 .NET 运行时）'
if ($Version) { Write-Host "  版本     : $Version" }
Write-Host ''

if ($Clean) {
    Write-Host '清理 bin / obj / publish / dist ...' -ForegroundColor DarkGray
    foreach ($name in @('bin', 'obj', 'publish', 'dist')) {
        $path = Join-Path $repoRoot $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    }
}

Write-Host '[1/2] 编译应用程序...' -ForegroundColor Cyan
& dotnet build $projectPath -c $Configuration --nologo
if ($LASTEXITCODE -ne 0) {
    throw "dotnet build 失败（退出码 $LASTEXITCODE）"
}

if ($SkipInstaller) {
    $stopwatch.Stop()
    Write-Host ''
    Write-Host "构建完成（已跳过安装包），耗时 $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒。" -ForegroundColor Green
    Write-Host "输出：$(Join-Path $repoRoot "bin\$Configuration\net11.0-windows")"
    return
}

Write-Host ''
Write-Host '[2/2] NativeAOT 发布并打包安装包...' -ForegroundColor Cyan

$installerArgs = @{
    Configuration = $Configuration
    Runtime       = $Runtime
}
if ($Version) { $installerArgs['Version'] = $Version }
if ($ISCCPath) { $installerArgs['ISCCPath'] = $ISCCPath }
if ($SkipPublish) { $installerArgs['SkipPublish'] = $true }

& $installerScript @installerArgs

$stopwatch.Stop()
$setupPath = $null
if ($Version) {
    $setupPath = Join-Path $distDir "NetConditioner-$Version-win-x64-Setup.exe"
}
if (-not $setupPath -or -not (Test-Path -LiteralPath $setupPath)) {
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
}
