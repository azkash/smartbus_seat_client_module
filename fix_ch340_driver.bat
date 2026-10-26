@echo off
:: BatchGotAdmin
:-------------------------------------
REM --> Check for permissions
IF "%PROCESSOR_ARCHITECTURE%" EQU "amd64" (
>nul 2>&1 "%SYSTEMROOT%\SysWOW64\cacls.exe" "%SYSTEMROOT%\SysWOW64\config\system"
) ELSE (
>nul 2>&1 "%SYSTEMROOT%\system32\cacls.exe" "%SYSTEMROOT%\system32\config\system"
)

REM --> If error flag set, we do not have admin.
if '%errorlevel%' NEQ '0' (
    echo Requesting administrative privileges...
    goto UACPrompt
) else ( goto gotAdmin )

:UACPrompt
    echo Set UAC = CreateObject^("Shell.Application"^) > "%temp%\getadmin.vbs"
    set params = %*:"=""
    echo UAC.ShellExecute "cmd.exe", "/c ""%~s0"" %params%", "", "runas", 1 >> "%temp%\getadmin.vbs"

    "%temp%\getadmin.vbs"
    del "%temp%\getadmin.vbs"
    exit /B

:gotAdmin
    pushd "%CD%"
    CD /D "%~dp0"
:--------------------------------------

echo ========================================================
echo   ROLLBACK DRIVER USB-SERIAL CH340 KE VERSI 3.4
echo ========================================================
echo.
echo 1. Menghapus driver versi bermasalah (v3.9.2024 / oem48.inf)...
pnputil /delete-driver oem48.inf /uninstall /force
echo.
echo 2. Memasang driver versi stabil (v3.4 / oem46.inf)...
pnputil /add-driver C:\Windows\INF\oem46.inf /install
echo.
echo 3. Memindai ulang perangkat (scan devices)...
pnputil /scan-devices
echo.
echo ========================================================
echo SELESAI!
echo Silakan cabut dan colokkan kembali kabel USB TTL Anda.
echo ========================================================
pause
