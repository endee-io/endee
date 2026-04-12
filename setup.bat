@echo off
setlocal

echo ==========================================
echo   📦 Box AI Engine: Master Setup
echo ==========================================
echo.

:: 1. Install Requirements
echo [1/4] Installing Python dependencies...
python -m pip install -r "%~dp0box\requirements.txt"
if %errorlevel% neq 0 (
    echo [!] Dependency installation failed. Check your Python/internet connection.
    pause
    exit /b 1
)

:: 2. Setup PATH
echo [2/4] Configuring System PATH...
powershell -Command " \
    $binDir = '%~dp0'; \
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User'); \
    if ($userPath -notlike '*'+$binDir+'*') { \
        [Environment]::SetEnvironmentVariable('Path', $userPath + ';' + $binDir, 'User'); \
        echo 'Added to User PATH.'; \
    } else { \
        echo 'Already in PATH.'; \
    }"

:: 3. Initial Indexing
echo [3/4] Initializing Codebase Index (Hybrid Search)...
python -m box.cli index
if %errorlevel% neq 0 (
    echo [!] Indexing failed. Is the Endee server running on port 8080?
)

:: 4. Finalizing
echo [4/4] Finalizing setup...
echo.
echo ==========================================
echo   🎉 Setup Complete!
echo ==========================================
echo.
echo 1. RESTART your terminal/PowerShell to use the 'box' command.
echo 2. Run 'box serve' to start the Intelligence brain.
echo 3. Open VSCode and use the Box Sidebar.
echo.
pause
exit /b 0
