@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
setlocal
set CONFIGURATION=%1
if "%CONFIGURATION%"=="" set CONFIGURATION=Debug

rem The WDK project is authoritative for Windows builds; require a VS/EWDK prompt.
where msbuild.exe >nul 2>nul
if errorlevel 1 (
    echo MSBuild was not found. Run this from a Visual Studio or EWDK build prompt.
    exit /b 1
)

msbuild "%~dp0driver\extfs.sln" /m /p:Configuration=%CONFIGURATION% /p:Platform=x64
exit /b %errorlevel%
