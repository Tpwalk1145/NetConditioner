@echo off
setlocal

rem NetConditioner one-key build wrapper: build.ps1 (build + package installer).
rem Usage: build.cmd [build.ps1 arguments]

set "PS=powershell.exe"
where pwsh >nul 2>nul && set "PS=pwsh.exe"

%PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "EXITCODE=%ERRORLEVEL%"

rem Keep the window open only when started by double-click (NC_NO_PAUSE=1 skips it).
if /i "%NC_NO_PAUSE%"=="1" exit /b %EXITCODE%
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul
if not errorlevel 1 pause

exit /b %EXITCODE%
