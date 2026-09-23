@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
setlocal EnableExtensions
cd /d "%~dp0"

set "MODE=%~1"
set "PLATFORM=%~2"
if "%MODE%"=="" set "MODE=setup"
if "%PLATFORM%"=="" set "PLATFORM=x64"
if /I "%PLATFORM%"=="arm64" set "PLATFORM=ARM64"
if /I not "%PLATFORM%"=="x64" if /I not "%PLATFORM%"=="ARM64" goto usage
if not exist "%~dp0VERSION" goto missingversion
set /p "PACKAGE_VERSION="<"%~dp0VERSION"
if "%PACKAGE_VERSION%"=="" goto missingversion

echo.
echo ============================================================
echo   ExtFS for Windows %PACKAGE_VERSION% - %PLATFORM% Build / Validate
echo ============================================================
echo.

where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: Windows PowerShell was not found.
    exit /b 1
)

if /I "%MODE%"=="driver" goto driveronly
if /I "%MODE%"=="setup" goto fullsetup
goto usage

:driveronly
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows\Build-And-Validate.ps1" -Configuration Release -Platform "%PLATFORM%"
goto result

:fullsetup
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows\Build-ExperimentalSetup.ps1" -Platform "%PLATFORM%"
goto result

:missingversion
echo ERROR: VERSION file is missing or empty.
exit /b 1

:usage
echo Usage:
echo   BUILD-EXTFS.cmd [setup^|driver] [x64^|ARM64]
exit /b 2

:result
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
    echo BUILD FAILED - exit code %RC%
    exit /b %RC%
)
if /I "%PLATFORM%"=="ARM64" (set "SLUG=arm64") else (set "SLUG=x64")
echo BUILD COMPLETED SUCCESSFULLY
if exist "%~dp0ExtFS-for-Windows-%PACKAGE_VERSION%-experimental-%SLUG%-setup.exe" (
    echo Installer: %~dp0ExtFS-for-Windows-%PACKAGE_VERSION%-experimental-%SLUG%-setup.exe
) else if exist "%~dp0release\driver\extfs.sys" (
    echo Driver: %~dp0release\driver\extfs.sys
)
exit /b 0
