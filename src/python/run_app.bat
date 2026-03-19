cd /d "F:\Front end\endee\src\python"

start cmd /k python -m uvicorn main:app --reload

timeout /t 3 >nul

start http://127.0.0.1:8000