@echo off
setlocal

echo [Box] Installing Box AI Engine locally...

:: Get the current directory
set "BIN_DIR=%~dp0"
set "BIN_DIR=%BIN_DIR:~0,-1%"

:: Check if already in PATH
echo %PATH% | findstr /i /c:"%BIN_DIR%" >nul
if %errorlevel% == 0 (
    echo [Box] Directory is already in PATH.
    goto :success
)

:: Add to user PATH using PowerShell (more robust than setx)
echo [Box] Adding %BIN_DIR% to User PATH...
powershell -Command "[Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path', 'User') + ';' + '%BIN_DIR%', 'User')"

if %errorlevel% neq 0 (
    echo [!] Failed to update PATH. Try running as Administrator.
    pause
    exit /b 1
)

:success
echo.
echo [Box] Installation complete!
echo [Box] IMPORTANT: Please RESTART your terminal/PowerShell to start using the 'box' command.
echo.
echo You can then use:
echo   box index
echo   box serve
echo.
pause
exit /b 0
