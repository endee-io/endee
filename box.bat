@echo off
setlocal
set "PYTHONPATH=%PYTHONPATH%;%~dp0"

:: Check if help was requested or no arguments
if "%~1" == "" (
    goto :usage
)

:: Pass all arguments to the box.cli module
python -m box.cli %*
exit /b %errorlevel%

:usage
echo Box Autonomous AI Engine
echo.
echo Usage:
echo   box index      - Scan and index the codebase
echo   box search     - Semantic/Hybrid lookup
echo   box serve      - Start the background API server
echo   box backup     - Manage snapshots
echo   box status     - System health check
echo   box build      - Run the dataset building pipeline
exit /b 0
