@echo off
setlocal

rem NetConditioner one-key build wrapper: build.ps1 (build + single-file exe + latest.json).
rem Usage: build.cmd [build.ps1 arguments] [-Pause]
rem Automation: no pause by default. Double-click: use build.cmd -Pause to keep the window open.

set "PS=powershell.exe"
where pwsh >nul 2>nul && set "PS=pwsh.exe"

rem -Pause is consumed by this wrapper (build.ps1 accepts it as a no-op for forwarding compatibility).
set "SHOULD_PAUSE="
for %%A in (%*) do if /i "%%~A"=="-Pause" set "SHOULD_PAUSE=1"

%PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "EXITCODE=%ERRORLEVEL%"

if defined SHOULD_PAUSE pause

exit /b %EXITCODE%
