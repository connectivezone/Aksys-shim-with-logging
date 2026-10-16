@echo off
setlocal enabledelayedexpansion
echo ============================================================================
echo  Akai S5000 / S6000 Windows 11 WinUSB Proxy Shim Builder (v10.1)
echo ============================================================================
echo.

echo [*] Closing any running aksys.exe instances to avoid DLL lock...
taskkill /F /IM aksys.exe >nul 2>nul

where cl.exe >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [*] Searching for Visual Studio x86 Native Tools environment...
    for %%Y in (2026 2022 2019 2017) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat" (
                call "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat"
                goto :compile
            )
            if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat" (
                call "%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat"
                goto :compile
            )
        )
    )
)

:compile
where cl.exe >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [!] Could not locate cl.exe.
    echo [!] Please open 'x86 Native Tools Command Prompt for VS' and run this script again.
    pause
    exit /b 1
)

echo [*] Compiling 32-bit version.dll proxy (v10.1)...
cl.exe /nologo /O2 /LD /EHsc akS56USB_WinUSB_Shim.cpp /link /OUT:version.dll /MACHINE:X86
if %ERRORLEVEL% neq 0 (
    echo.
    echo [!] Compilation failed. Make sure you run this from an x86 Native Tools Command Prompt.
    pause
    exit /b 1
)

echo.
echo [+] SUCCESS! Built 32-bit version.dll (v10.1).
echo.
echo ============================================================================
echo  INSTALLATION INSTRUCTIONS:
echo ============================================================================
echo  1. In 'C:\Program Files (x86)\AKAIPRO\aksys\devices\':
echo     Ensure original S56Device.dll is present.
echo  2. Copy this newly built 'version.dll' into:
echo     C:\Program Files (x86)\AKAIPRO\aksys\devices\
echo  3. Launch aksys.exe!
echo ============================================================================
pause
