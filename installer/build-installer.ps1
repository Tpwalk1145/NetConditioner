#Requires -Version 5.1
<#
.SYNOPSIS
    Build the NetConditioner Windows installer.

.DESCRIPTION
    1. cargo build（Release）并暂存单文件 exe + WinDivert 文件到 publish\win-x64。
    2. 调用 Inno Setup 6 的 ISCC.exe 编译 installer\NetConditioner.iss。
    3. 生成 dist\NetConditioner-<version>-win-x64-Setup.exe 与 dist\latest.json。

    依赖：Rust MSVC 工具链、Inno Setup 6（winget install JRSoftware.InnoSetup）。
    ISCC.exe 会从 PATH、INNO_SETUP_HOME 或默认安装目录查找，也可用 -ISCCPath 指定。

.EXAMPLE
    .\installer\build-installer.ps1

.EXAMPLE
    .\installer\build-installer.ps1 -SkipPublish -Version 2.0.1
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Version,
    [string]$ISCCPath,
    [string]$UpdateBaseUrl,
    [string]$Notes,
    [switch]$SkipPublish
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$cargoToml = Join-Path $repoRoot 'Cargo.toml'
$payloadDir = Join-Path $repoRoot 'publish\win-x64'
$distDir = Join-Path $repoRoot 'dist'
$issPath = Join-Path $PSScriptRoot 'NetConditioner.iss'
$targetDir = Join-Path $repoRoot (Join-Path 'target' $(if ($Configuration -eq 'Release') { 'release' } else { 'debug' }))

function Resolve-Iscc {
    param([string]$Explicit)

    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) {
            return (Resolve-Path -LiteralPath $Explicit).Path
        }
        throw "ISCC.exe not found: $Explicit"
    }

    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @()
    if ($env:INNO_SETUP_HOME) {
        $candidates += (Join-Path $env:INNO_SETUP_HOME 'ISCC.exe')
    }
    $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    $programFiles = [Environment]::GetFolderPath('ProgramFiles')
    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    if ($programFilesX86) {
        $candidates += (Join-Path $programFilesX86 'Inno Setup 6\ISCC.exe')
    }
    if ($programFiles) {
        $candidates += (Join-Path $programFiles 'Inno Setup 6\ISCC.exe')
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    throw 'ISCC.exe was not found. Install Inno Setup 6 (winget install JRSoftware.InnoSetup) or pass -ISCCPath.'
}

if (-not $Version) {
    $match = Select-String -LiteralPath $cargoToml -Pattern '^\s*version\s*=\s*"([^"]+)"' | Select-Object -First 1
    if ($match) { $Version = $match.Matches[0].Groups[1].Value } else { $Version = '0.0.0' }
}

# VersionInfoVersion in the .iss must be a 4-part numeric version.
$numericParts = @()
foreach ($part in ($Version -split '\.')) {
    $digits = ($part -replace '[^0-9].*$', '')
    if (-not $digits) { $digits = '0' }
    $numericParts += $digits
}
while ($numericParts.Count -lt 4) { $numericParts += '0' }
$appVersionNumeric = ($numericParts[0..3] -join '.')

Write-Host 'NetConditioner installer build'
Write-Host "  Cargo.toml : $cargoToml"
Write-Host "  Version    : $Version"
Write-Host "  Config     : $Configuration"
Write-Host "  Payload    : $payloadDir"
Write-Host "  Output     : $distDir"
Write-Host ''

if (-not $SkipPublish) {
    Write-Host '[1/3] Building native executable...'
    $cargoArgs = @('build')
    if ($Configuration -eq 'Release') { $cargoArgs += '--release' }
    & cargo @cargoArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cargo build failed with exit code $LASTEXITCODE"
    }

    if (Test-Path -LiteralPath $payloadDir) {
        Remove-Item -LiteralPath $payloadDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $payloadDir 'third_party\WinDivert') | Out-Null

    $executable = Join-Path $targetDir 'NetConditioner.exe'
    if (-not (Test-Path -LiteralPath $executable)) {
        throw "Executable not found: $executable"
    }
    Copy-Item -LiteralPath $executable -Destination (Join-Path $payloadDir 'NetConditioner.exe') -Force

    $thirdParty = Join-Path $repoRoot 'third_party\WinDivert'
    foreach ($name in @('WinDivert.dll', 'WinDivert64.sys')) {
        Copy-Item -LiteralPath (Join-Path $thirdParty $name) -Destination (Join-Path $payloadDir $name) -Force
    }
    foreach ($name in @('LICENSE', 'VERSION', 'README.txt')) {
        Copy-Item -LiteralPath (Join-Path $thirdParty $name) -Destination (Join-Path $payloadDir "third_party\WinDivert\$name") -Force
    }
} else {
    Write-Host '[1/3] Skipping build (-SkipPublish).'
}

Write-Host '[2/3] Validating payload...'
$requiredFiles = @(
    'NetConditioner.exe',
    'WinDivert.dll',
    'WinDivert64.sys'
)
foreach ($name in $requiredFiles) {
    $path = Join-Path $payloadDir $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Payload is missing required file: $path"
    }
}

$payloadBytes = (Get-ChildItem -LiteralPath $payloadDir -Recurse -File | Measure-Object -Sum Length).Sum
Write-Host ("  Payload : {0:N2} MB" -f ($payloadBytes / 1MB))

$iscc = Resolve-Iscc -Explicit $ISCCPath
Write-Host "  ISCC    : $iscc"

Write-Host '[3/3] Compiling installer...'
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
& $iscc "/DAppVersion=$Version" "/DAppVersionNumeric=$appVersionNumeric" $issPath
if ($LASTEXITCODE -ne 0) {
    throw "ISCC.exe failed with exit code $LASTEXITCODE"
}

$setupPath = Join-Path $distDir "NetConditioner-$Version-win-x64-Setup.exe"
if (-not (Test-Path -LiteralPath $setupPath)) {
    throw "Installer output was not found: $setupPath"
}

$setup = Get-Item -LiteralPath $setupPath
$hash = Get-FileHash -LiteralPath $setupPath -Algorithm SHA256
Write-Host ''
Write-Host 'Installer built successfully:'
Write-Host "  Path   : $($setup.FullName)"
Write-Host "  Size   : $([math]::Round($setup.Length / 1MB, 2)) MB"
Write-Host "  SHA256 : $($hash.Hash)"

if (-not $UpdateBaseUrl) {
    $UpdateBaseUrl = 'https://example.com/NetConditioner'
    Write-Host ''
    Write-Warning '未提供 -UpdateBaseUrl：latest.json 使用占位地址，上传前请改成真实地址。'
}
$baseUrl = $UpdateBaseUrl.TrimEnd('/')
if (-not $Notes) { $Notes = "版本 $Version" }

$manifest = [ordered]@{
    schemaVersion = 1
    version       = $Version
    publishedAt   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    notes         = $Notes
    pageUrl       = "$baseUrl/"
    installer     = [ordered]@{
        url    = "$baseUrl/$($setup.Name)"
        sha256 = $hash.Hash
        size   = $setup.Length
    }
}

$manifestPath = Join-Path $distDir 'latest.json'
$manifestJson = ($manifest | ConvertTo-Json -Depth 5) + [Environment]::NewLine
[System.IO.File]::WriteAllText(
    $manifestPath, $manifestJson, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ''
Write-Host 'Update manifest generated:'
Write-Host "  Path   : $manifestPath"
Write-Host "  URL    : $baseUrl/$($setup.Name)"
Write-Host "  SHA256 : $($hash.Hash)"
Write-Host '  上传时把 Setup exe 和 latest.json 放在同一目录。'

