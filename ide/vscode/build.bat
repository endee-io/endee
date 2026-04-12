@echo off
setlocal

echo [Box] Compiling VSCode Extension...

cd /d "%~dp0"

:: 1. Check for npm
where npm >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] npm not found. Please install Node.js from https://nodejs.org
    pause
    exit /b 1
)

:: 2. Install dependencies
echo [Box] Installing Node.js dependencies...
call npm install
if %errorlevel% neq 0 (
    echo [!] npm install failed.
    pause
    exit /b 1
)

:: 3. Compile
echo [Box] Building TypeScript...
call npm run compile
if %errorlevel% neq 0 (
    echo [!] Build failed.
    pause
    exit /b 1
)

echo.
echo [Box] Extension built successfully!
echo [Box] You can now load this folder into VSCode or package it for Open VSX.
echo.
pause
exit /b 0
