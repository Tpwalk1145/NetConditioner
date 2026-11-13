#Requires -Version 5.1
<#
.SYNOPSIS
    Build the NetConditioner Windows installer.

.DESCRIPTION
    1. dotnet publish the WinForms app for win-x64 with NativeAOT
       (single native exe; the target machine needs no .NET runtime).
    2. Compile installer\NetConditioner.iss with Inno Setup 6 (ISCC.exe).
    3. Write dist\NetConditioner-<version>-win-x64-Setup.exe.

    Inno Setup 6 is required: winget install JRSoftware.InnoSetup
    ISCC.exe is located on PATH, under INNO_SETUP_HOME, or in the default
    per-user / per-machine install folders. Use -ISCCPath to override.

.EXAMPLE
    .\installer\build-installer.ps1

.EXAMPLE
    .\installer\build-installer.ps1 -SkipPublish -Version 1.0.1
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Runtime = 'win-x64',
    [string]$Version,
    [string]$ISCCPath,
    [string]$UpdateBaseUrl,
    [string]$Notes,
    [switch]$SkipPublish
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $repoRoot 'NetConditioner.csproj'
$payloadDir = Join-Path $repoRoot 'publish\win-x64-selfcontained'
$distDir = Join-Path $repoRoot 'dist'
$issPath = Join-Path $PSScriptRoot 'NetConditioner.iss'

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
    $perUser = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
    $candidates += $perUser
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
    [xml]$projectXml = Get-Content -LiteralPath $projectPath -Raw
    $versionElement = $projectXml.Project.PropertyGroup.Version | Select-Object -First 1
    if ($versionElement) { $Version = [string]$versionElement } else { $Version = '1.0.0' }
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
Write-Host "  Project : $projectPath"
Write-Host "  Version : $Version"
Write-Host "  Runtime : $Runtime (NativeAOT, self-contained)"
Write-Host "  Payload : $payloadDir"
Write-Host "  Output  : $distDir"
Write-Host ''

if (-not $SkipPublish) {
    Write-Host '[1/3] Publishing NativeAOT application...'
    if (Test-Path -LiteralPath $payloadDir) {
        Remove-Item -LiteralPath $payloadDir -Recurse -Force
    }
    # PublishProfile=Aot 打开 PublishAot + StripSymbols；不能写进 csproj，
    # 否则 dotnet build 会因 Windows 窗体 + 剪裁检查（NETSDK1175）而失败。
    # --disable-build-servers：避免 SDK 等待 MSBuild 服务器连接超时（约 8 秒）并打印警告。
    & dotnet publish $projectPath -c $Configuration -r $Runtime -p:PublishProfile=Aot -p:DebugType=None -p:DebugSymbols=false -o $payloadDir --disable-build-servers
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet publish failed with exit code $LASTEXITCODE"
    }
}
else {
    Write-Host '[1/3] Skipping publish (-SkipPublish).'
}

Write-Host '[2/3] Validating payload...'
# NativeAOT 发布只产出原生 exe（没有 runtimeconfig.json / deps.json）。
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
