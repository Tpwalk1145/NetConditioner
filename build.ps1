#Requires -Version 5.1
<#
.SYNOPSIS
    NetConditioner 一键构建：LLVM 工具链 + 单元测试 + 生成单文件 exe 与云更新清单。

.DESCRIPTION
    默认按顺序执行：
      1. 用 clang-cl.exe 编译 C23 源码，lld-link.exe 链接，llvm-rc.exe 编译资源；
         可用 -Toolchain MSVC 切回 cl.exe/link.exe/rc.exe
      2. 运行单元测试（tests 下的控制台测试程序，无需管理员、不联网）
      3. 把 build\NetConditioner.exe 复制为 dist\NetConditioner-<版本>-win-x64.exe，
         并生成 dist\latest.json

    版本始终取自 version.txt；-Version 只能原样确认该版本。
    Debug 构建只输出到 dist\debug\，不会生成或更新 latest.json。
    app.manifest 的 assemblyIdentity.version 必须与 version.txt 同步（不足四位补 0）。
    依赖：完整的 LLVM/Clang（clang-cl.exe、lld-link.exe、llvm-rc.exe）、
    Visual Studio 2022 Build Tools（提供 Windows 头文件与库）与 Windows SDK。
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateSet('LLVM', 'Clang', 'MSVC')]
    [string]$Toolchain = 'LLVM',
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

# Clang 作为旧名称兼容，与完整 LLVM 工具链等价。
if ($Toolchain -eq 'Clang') {
    $Toolchain = 'LLVM'
}

$repoRoot = $PSScriptRoot
$versionFile = Join-Path $repoRoot 'version.txt'
$appManifest = Join-Path $repoRoot 'app.manifest'
$srcDir = Join-Path $repoRoot 'src'
$testsDir = Join-Path $repoRoot 'tests'
$buildDir = Join-Path $repoRoot 'build'
$objDir = Join-Path $buildDir 'obj'
$testObjDir = Join-Path $buildDir 'obj-tests'
$testBinDir = Join-Path $buildDir 'tests'
$genDir = Join-Path $buildDir 'gen'
$distDir = Join-Path $repoRoot 'dist'
$winDivertDir = Join-Path $repoRoot 'third_party\WinDivert'
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

function Write-Utf8File {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
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

function Import-MsvcEnvironment {
    $candidates = @()
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    if ($programFilesX86) {
        $candidates += (Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe')
    }
    if ($programFiles) {
        $candidates += (Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    }
    $vswhere = $null
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            $vswhere = $candidate
            break
        }
    }
    if (-not $vswhere) {
        throw '未找到 vswhere.exe。请安装 Visual Studio 2022 Build Tools（包含“使用 C++ 的桌面开发”）。'
    }
    $installs = @(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if ($installs.Count -eq 0 -or -not $installs[0]) {
        throw '未找到 Visual Studio C++ 工具集。请安装 VS2022 Build Tools 的“使用 C++ 的桌面开发”工作负载。'
    }
    $vcvars = Join-Path $installs[0].Trim() 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "未找到 vcvars64.bat：$vcvars"
    }
    $command = 'call "' + $vcvars + '" >nul 2>&1 && set'
    $lines = @(& cmd.exe /d /c $command)
    if ($LASTEXITCODE -ne 0) {
        throw '导入 Visual Studio 构建环境失败。'
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            $name = $Matches[1]
            $value = $Matches[2]
            Set-Item -Path ('env:' + $name) -Value $value
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw '导入 vcvars64 后仍找不到 cl.exe。'
    }
}

function ConvertTo-ProcessArgument {
    param([string]$Value)
    if ($Value -match '[\s"]') {
        return '"' + $Value + '"'
    }
    return $Value
}

# clang-cl does not implement /MP. Compile translation units as separate
# processes and keep up to min(CPU count, 12) of them running at once.
function Invoke-ClangCompile {
    param(
        [string[]]$Files,
        [string]$TargetDir,
        [string[]]$Flags,
        [string]$Compiler
    )
    if (-not (Test-Path -LiteralPath $TargetDir)) {
        New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
    }
    $objects = New-Object System.Collections.Generic.List[string]
    $jobs = New-Object System.Collections.Generic.List[object]
    $failures = New-Object System.Collections.Generic.List[string]
    $limit = [Environment]::ProcessorCount
    if ($limit -lt 1) { $limit = 1 }
    if ($limit -gt 12) { $limit = 12 }
    $index = 0
    while ($index -lt $Files.Count -or $jobs.Count -gt 0) {
        while ($jobs.Count -lt $limit -and $index -lt $Files.Count) {
            $file = $Files[$index]
            $index++
            if ($file.Length -le $repoRoot.Length) {
                throw "源文件路径异常：$file"
            }
            $name = [System.IO.Path]::GetFileNameWithoutExtension($file)
            $object = Join-Path $TargetDir ($name + '.obj')
            $objects.Add($object) | Out-Null
            $arguments = @($Flags) + @("/Fo$object", $file)
            $argumentLine = ($arguments | ForEach-Object { ConvertTo-ProcessArgument $_ }) -join ' '
            $startInfo = New-Object System.Diagnostics.ProcessStartInfo
            $startInfo.FileName = $Compiler
            $startInfo.Arguments = $argumentLine
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $process = New-Object System.Diagnostics.Process
            $process.StartInfo = $startInfo
            if (-not $process.Start()) {
                throw "无法启动编译器：$Compiler"
            }
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()
            $jobs.Add([pscustomobject]@{
                Name = $name
                Process = $process
                Stdout = $stdoutTask
                Stderr = $stderrTask
            }) | Out-Null
        }
        if ($jobs.Count -eq 0) {
            break
        }
        $finished = @($jobs | Where-Object { $_.Process.HasExited })
        if ($finished.Count -eq 0) {
            Start-Sleep -Milliseconds 15
            continue
        }
        foreach ($job in $finished) {
            $job.Process.WaitForExit()
            $exitCode = $job.Process.ExitCode
            $stdoutText = $job.Stdout.GetAwaiter().GetResult()
            $stderrText = $job.Stderr.GetAwaiter().GetResult()
            if ($stdoutText -and $stdoutText.Trim().Length -gt 0) {
                Write-Host $stdoutText.TrimEnd()
            }
            if ($stderrText -and $stderrText.Trim().Length -gt 0) {
                Write-Host $stderrText.TrimEnd()
            }
            if ($exitCode -ne 0) {
                $failures.Add($job.Name) | Out-Null
            }
            $job.Process.Dispose()
            [void]$jobs.Remove($job)
        }
    }
    if ($failures.Count -gt 0) {
        throw "编译失败：$($failures -join ', ')"
    }
    return $objects.ToArray()
}

function Invoke-ClCompile {
    param(
        [string[]]$Files,
        [string]$TargetDir,
        [string[]]$Flags
    )
    if (-not (Test-Path -LiteralPath $TargetDir)) {
        New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
    }
    $objects = New-Object System.Collections.Generic.List[string]
    foreach ($file in $Files) {
        if ($file.Length -le $repoRoot.Length) {
            throw "源文件路径异常：$file"
        }
        $name = [System.IO.Path]::GetFileNameWithoutExtension($file)
        $object = Join-Path $TargetDir ($name + '.obj')
        $objects.Add($object) | Out-Null
    }
    # One cl invocation with /MP: the compiler schedules translation units across
    # cores and /Fo<dir> keeps every object in the same output directory.
    $compileOutput = & cl.exe @Flags "/Fo$TargetDir\" @Files 2>&1
    if ($compileOutput) {
        $compileOutput | ForEach-Object { Write-Host $_ }
    }
    if ($LASTEXITCODE -ne 0) {
        throw "编译失败（退出码 $LASTEXITCODE）"
    }
    return $objects.ToArray()
}

function Invoke-Link {
    param(
        [string[]]$Objects,
        [string]$Resource,
        [string]$Output,
        [string]$Subsystem,
        [string[]]$ExtraFlags
    )
    $libraries = @(
        'kernel32.lib', 'user32.lib', 'gdi32.lib', 'comctl32.lib', 'shell32.lib',
        'ole32.lib', 'advapi32.lib', 'bcrypt.lib', 'winhttp.lib', 'ws2_32.lib',
        'iphlpapi.lib', 'shlwapi.lib', 'uuid.lib'
    )
    $arguments = @('/NOLOGO', "/SUBSYSTEM:$Subsystem", "/OUT:$Output", '/OPT:REF', '/OPT:ICF', '/STACK:8388608')
    if ($Toolchain -eq 'MSVC') {
        $arguments += '/MANIFEST:NO'
    }
    if ($ExtraFlags) {
        $arguments += $ExtraFlags
    }
    $arguments += $Objects
    if ($Resource) {
        $arguments += $Resource
    }
    $arguments += $libraries
    & $linker @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "链接失败（退出码 $LASTEXITCODE）：$Output"
    }
}

Write-Host '========================================' -ForegroundColor Cyan
Write-Host ' NetConditioner 构建（C / LLVM，单文件版）' -ForegroundColor Cyan
Write-Host '========================================' -ForegroundColor Cyan

if (-not (Test-Path -LiteralPath $versionFile)) {
    throw "未找到版本文件：$versionFile"
}
if (-not (Test-Path -LiteralPath $appManifest)) {
    throw "未找到应用程序清单：$appManifest"
}
if (-not (Test-Path -LiteralPath $srcDir)) {
    throw "未找到源码目录：$srcDir"
}

$rawVersion = [System.IO.File]::ReadAllText($versionFile).Trim()
$canonicalVersion = Get-CanonicalVersion -Value $rawVersion
if (-not $canonicalVersion) {
    throw "version.txt 版本格式无效：$rawVersion（要求 2-4 个数值段，每段不超过 Int32）"
}
if ($PSBoundParameters.ContainsKey('Version')) {
    if (-not $Version) { throw '-Version 不能为空；省略该参数即使用 version.txt 版本。' }
    if ((Get-CanonicalVersion -Value $Version) -cne $canonicalVersion) {
        throw "-Version 必须与 version.txt 的规范版本完全一致：$canonicalVersion（实际：$Version）"
    }
}
$Version = $canonicalVersion
$versionParts = @($Version.Split('.') + @('0', '0', '0', '0'))[0..3]
$expectedManifestVersion = $versionParts -join '.'

[xml]$manifestXml = Get-Content -LiteralPath $appManifest -Raw
$identity = $manifestXml.SelectSingleNode('/*[local-name()="assembly"]/*[local-name()="assemblyIdentity"]')
if (-not $identity) { throw "app.manifest 缺少 assemblyIdentity：$appManifest" }
if ($identity.GetAttribute('version') -cne $expectedManifestVersion) {
    throw "app.manifest assemblyIdentity.version 必须为 $expectedManifestVersion（与 version.txt $Version 同步）"
}

Import-MsvcEnvironment

if ($Toolchain -eq 'LLVM') {
    $clangCommand = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    $lldCommand = Get-Command lld-link.exe -ErrorAction SilentlyContinue
    $llvmRcCommand = Get-Command llvm-rc.exe -ErrorAction SilentlyContinue
    if (-not $clangCommand) {
        throw '未找到 clang-cl.exe。请安装 LLVM/Clang 并把其 bin 目录加入 PATH，或使用 -Toolchain MSVC。'
    }
    if (-not $lldCommand) {
        throw '未找到 lld-link.exe。请安装完整的 LLVM/Clang 工具链，或使用 -Toolchain MSVC。'
    }
    if (-not $llvmRcCommand) {
        throw '未找到 llvm-rc.exe。请安装完整的 LLVM/Clang 工具链，或使用 -Toolchain MSVC。'
    }
    $compiler = $clangCommand.Source
    $linker = $lldCommand.Source
    $resourceCompiler = $llvmRcCommand.Source
    $compilerVersion = (& $compiler --version | Select-Object -First 1)
    if (-not $compilerVersion) { $compilerVersion = 'clang-cl' }
} else {
    $clCommand = Get-Command cl.exe -ErrorAction SilentlyContinue
    $linkCommand = Get-Command link.exe -ErrorAction SilentlyContinue
    $rcCommand = Get-Command rc.exe -ErrorAction SilentlyContinue
    if (-not $clCommand) {
        throw '未找到 cl.exe。请安装 Visual Studio 2022 Build Tools 的“使用 C++ 的桌面开发”工作负载。'
    }
    if (-not $linkCommand) {
        throw '未找到 link.exe。请安装 Visual Studio 2022 Build Tools 的 C++ 工具集。'
    }
    if (-not $rcCommand) {
        throw '未找到 rc.exe。请安装 Windows SDK。'
    }
    $compiler = $clCommand.Source
    $linker = $linkCommand.Source
    $resourceCompiler = $rcCommand.Source
    $compilerVersion = 'MSVC cl.exe'
}

$winDivertVersion = [System.IO.File]::ReadAllText((Join-Path $winDivertDir 'VERSION')).Trim()
Write-Host ('  工具链   : ' + $Toolchain)
Write-Host ('  编译器   : ' + $compilerVersion + ' (' + $compiler + ')')
Write-Host ('  链接器   : ' + $linker)
Write-Host ('  资源编译 : ' + $resourceCompiler)
Write-Host ('  配置     : ' + $Configuration)
Write-Host ('  版本     : ' + $Version + '（version.txt）')
Write-Host ('  WinDivert: ' + $winDivertVersion)
Write-Host ''

if ($Clean) {
    Write-Host '清理 build / dist ...' -ForegroundColor DarkGray
    foreach ($name in @('build', 'dist')) {
        $path = Join-Path $repoRoot $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        } catch {
            Write-Warning "无法完全删除 $path（可能仍有 NetConditioner 在运行）：$($_.Exception.Message)"
        }
    }
}

foreach ($directory in @($objDir, $testObjDir, $testBinDir, $genDir)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

# ---------------------------------------------------------------------------
# 生成版本头与资源脚本
# ---------------------------------------------------------------------------

$newline = [Environment]::NewLine
$versionHeader = @"
#pragma once
#define NC_VERSION_STR "$Version"
#define NC_VERSION_WSTR L"$Version"
#define NC_VERSION_MAJOR $($versionParts[0])
#define NC_VERSION_MINOR $($versionParts[1])
#define NC_VERSION_PATCH $($versionParts[2])
#define NC_VERSION_BUILD $($versionParts[3])
#define NC_VERSION_NUMERIC $($versionParts[0]),$($versionParts[1]),$($versionParts[2]),$($versionParts[3])
#define NC_VERSION_FOUR "$($versionParts[0]).$($versionParts[1]).$($versionParts[2]).$($versionParts[3])"
#define NC_WINDIVERT_VERSION "$winDivertVersion"
#define NC_WINDIVERT_VERSION_WSTR L"$winDivertVersion"
"@
Write-Utf8File -Path (Join-Path $genDir 'nc_version.h') -Text ($versionHeader + $newline)

function ConvertTo-RcPath {
    param([string]$Path)
    return $Path.Replace('\', '\\')
}

$manifestRc = ConvertTo-RcPath (Join-Path $repoRoot 'app.manifest')
$dllRc = ConvertTo-RcPath (Join-Path $winDivertDir 'WinDivert.dll')
$sysRc = ConvertTo-RcPath (Join-Path $winDivertDir 'WinDivert64.sys')
$licenseRc = ConvertTo-RcPath (Join-Path $winDivertDir 'LICENSE')
$payloadVersionRc = ConvertTo-RcPath (Join-Path $winDivertDir 'VERSION')

$resourceScript = @"
#pragma code_page(65001)
1 24 "$manifestRc"
1 VERSIONINFO
FILEVERSION $($versionParts[0]),$($versionParts[1]),$($versionParts[2]),$($versionParts[3])
PRODUCTVERSION $($versionParts[0]),$($versionParts[1]),$($versionParts[2]),$($versionParts[3])
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "NetConditioner"
            VALUE "FileDescription", "NetConditioner"
            VALUE "FileVersion", "$($versionParts[0]).$($versionParts[1]).$($versionParts[2]).$($versionParts[3])"
            VALUE "InternalName", "NetConditioner"
            VALUE "OriginalFilename", "NetConditioner.exe"
            VALUE "ProductName", "NetConditioner"
            VALUE "ProductVersion", "$Version"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
WINDIVERT_DLL RCDATA "$dllRc"
WINDIVERT_SYS RCDATA "$sysRc"
WINDIVERT_LICENSE RCDATA "$licenseRc"
WINDIVERT_VERSION RCDATA "$payloadVersionRc"
"@
$resourcePath = Join-Path $genDir 'netconditioner.rc'
Write-Utf8File -Path $resourcePath -Text ($resourceScript + $newline)

# ---------------------------------------------------------------------------
# 编译与链接
# ---------------------------------------------------------------------------

$defines = @(
    '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00', '/DWIN32_LEAN_AND_MEAN',
    '/D_CRT_SECURE_NO_WARNINGS'
)
$stdFlag = if ($Toolchain -eq 'LLVM') { '/clang:-std=c23' } else { '/std:clatest' }
$commonFlags = @('/nologo', '/c', $stdFlag, '/W4', '/wd5105', '/MT', '/utf-8', '/Zp8') + $defines +
    @('/I', $srcDir, '/I', $genDir)
if ($Toolchain -eq 'MSVC') {
    $commonFlags += '/MP'
}
$linkFlags = @()
if ($Configuration -eq 'Release') {
    $commonFlags += '/O2'
    if ($Toolchain -eq 'MSVC') {
        $commonFlags += '/GL'
        $linkFlags += '/LTCG'
    }
} else {
    $commonFlags += @('/Od', '/Zi')
    if ($Toolchain -eq 'MSVC') {
        $commonFlags += '/RTC1'
    }
    $linkFlags += @('/DEBUG', "/PDB:$buildDir\NetConditioner.pdb")
}

$sourceFiles = @(Get-ChildItem -LiteralPath $srcDir -Recurse -Filter *.c -File |
    Sort-Object FullName | ForEach-Object { $_.FullName })
if ($sourceFiles.Count -eq 0) {
    throw "源码目录中没有 .c 文件：$srcDir"
}
$mainSource = Join-Path $srcDir 'main.c'
$librarySources = @($sourceFiles | Where-Object { $_ -ne $mainSource })

Write-Host '[1/4] 编译应用程序...' -ForegroundColor Cyan
if ($Toolchain -eq 'LLVM') {
    $mainObject = @(Invoke-ClangCompile -Files @($mainSource) -TargetDir $objDir -Flags $commonFlags -Compiler $compiler)[0]
    $libraryObjects = @(Invoke-ClangCompile -Files $librarySources -TargetDir $objDir -Flags $commonFlags -Compiler $compiler)
} else {
    $mainObject = @(Invoke-ClCompile -Files @($mainSource) -TargetDir $objDir -Flags $commonFlags)[0]
    $libraryObjects = @(Invoke-ClCompile -Files $librarySources -TargetDir $objDir -Flags $commonFlags)
}

Write-Host '      编译资源...' -ForegroundColor DarkGray
$resourceObject = Join-Path $objDir 'netconditioner.res'
if ($Toolchain -eq 'LLVM') {
    & $resourceCompiler /FO $resourceObject /I $genDir $resourcePath
} else {
    & rc.exe /nologo /i $genDir "/fo$resourceObject" $resourcePath
}
if ($LASTEXITCODE -ne 0) { throw "资源编译失败（退出码 $LASTEXITCODE）" }

$executable = Join-Path $buildDir 'NetConditioner.exe'
Invoke-Link -Objects (@($mainObject) + $libraryObjects) -Resource $resourceObject -Output $executable -Subsystem Windows -ExtraFlags $linkFlags

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------

if (-not $SkipTests) {
    Write-Host ''
    Write-Host '[2/4] 编译并运行单元测试...' -ForegroundColor Cyan
    if (-not (Test-Path -LiteralPath $testsDir)) {
        throw "未找到测试目录：$testsDir"
    }
    $testSources = @(Get-ChildItem -LiteralPath $testsDir -Filter *.c -File |
        Sort-Object FullName | ForEach-Object { $_.FullName })
    if ($testSources.Count -eq 0) {
        throw "测试目录中没有 .c 文件：$testsDir"
    }
    if ($Toolchain -eq 'LLVM') {
        $testObjects = Invoke-ClangCompile -Files $testSources -TargetDir $testObjDir -Flags $commonFlags -Compiler $compiler
    } else {
        $testObjects = Invoke-ClCompile -Files $testSources -TargetDir $testObjDir -Flags $commonFlags
    }
    $testExecutable = Join-Path $testBinDir 'NetConditionerTests.exe'
    Invoke-Link -Objects (@($libraryObjects) + $testObjects) -Resource $null -Output $testExecutable -Subsystem Console -ExtraFlags $linkFlags
    & $testExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "单元测试失败（退出码 $LASTEXITCODE）"
    }
} else {
    Write-Host ''
    Write-Host '[2/4] 已跳过单元测试（-SkipTests）。' -ForegroundColor DarkGray
}

if ($SkipPackage) {
    $stopwatch.Stop()
    Write-Host ''
    Write-Host "构建完成（已跳过打包），耗时 $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) 秒。" -ForegroundColor Green
    Write-Host "输出：$executable"
    return
}

# ---------------------------------------------------------------------------
# 打包
# ---------------------------------------------------------------------------

if ($Configuration -eq 'Release') {
    if (-not $UpdateBaseUrl) {
        throw '未提供 -UpdateBaseUrl，无法生成 latest.json 中的下载地址。'
    }
    $baseUrl = $UpdateBaseUrl.TrimEnd('/')
    if (-not $PageUrl) { $PageUrl = $baseUrl }
    if (-not $Notes) { $Notes = "版本 $Version" }
}

Write-Host ''
Write-Host '[3/4] 校验 x64 产物...' -ForegroundColor Cyan
Assert-X64Executable -Path $executable

Write-Host '[4/4] 生成单文件产物...' -ForegroundColor Cyan
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