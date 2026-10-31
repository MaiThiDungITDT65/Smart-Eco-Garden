@echo off
setlocal
cd /d "%~dp0"
if not exist "backend\.venv\Scripts\python.exe" (
 echo [LOI] Chua cai Backend. Chay CaiDat_Windows.bat truoc.
 pause
 exit /b 1
)
if not exist "frontend\node_modules" (
 echo [LOI] Chua cai Frontend. Chay CaiDat_Windows.bat truoc.
 pause
 exit /b 1
)
start "Garden Backend" /D "%~dp0backend" cmd /k ".venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8000"
start "Garden NextJS" /D "%~dp0frontend" cmd /k "npm run dev"
echo.
echo Laptop:  http://localhost:3000
 echo History: http://localhost:3000/history
 echo Phone:   http://IP_LAPTOP:3000 ^(same SmartEcoGarden WiFi^)
 echo Backend: http://127.0.0.1:8000/api/health
 echo ESP32:   http://192.168.4.1/data
pause
