@echo off
REM
REM Install or uninstall the usbip-broker service.
REM
REM Usage:
REM   install.cmd                  -- install + start
REM   install.cmd /uninstall       -- stop + remove
REM
REM Must be run from an elevated Administrator command prompt.
REM
setlocal enabledelayedexpansion

set "EXE=%~dp0usbip-broker.exe"

if not exist "%EXE%" (
    echo [error] usbip-broker.exe not found at "%EXE%"
    exit /b 1
)

if /i "%~1"=="/uninstall" (
    "%EXE%" --uninstall
    if errorlevel 1 (
        echo [error] uninstall failed
        exit /b 1
    )
    echo usbip-broker uninstalled.
    exit /b 0
)

if not exist "%ProgramData%\USBip" mkdir "%ProgramData%\USBip"
if not exist "%ProgramData%\USBip\policy.json" (
    copy /y "%~dp0policy.example.json" "%ProgramData%\USBip\policy.json" >nul
)
if not exist "%ProgramData%\USBip\owners.json" (
    copy /y "%~dp0owners.example.json" "%ProgramData%\USBip\owners.json" >nul
)

REM Restrict policy.json: SYSTEM and Administrators full, authenticated users read.
icacls "%ProgramData%\USBip\policy.json" /inheritance:r ^
    /grant:r "SYSTEM:(F)" "BUILTIN\Administrators:(F)" "Authenticated Users:(R)" >nul
icacls "%ProgramData%\USBip\owners.json" /inheritance:r ^
    /grant:r "SYSTEM:(F)" "BUILTIN\Administrators:(F)" "Authenticated Users:(R)" >nul

"%EXE%" --install
if errorlevel 1 (
    echo [error] install failed
    exit /b 1
)
echo usbip-broker installed.
exit /b 0
