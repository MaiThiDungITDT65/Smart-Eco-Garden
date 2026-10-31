@echo off
setlocal
cd /d "%~dp0"
echo ============================================
echo Cai dat Smart Eco Garden Dashboard
 echo ============================================
where python >nul 2>&1 || (echo [LOI] Chua co Python trong PATH & pause & exit /b 1)
where npm >nul 2>&1 || (echo [LOI] Chua co Node.js/npm trong PATH & pause & exit /b 1)
if not exist "backend\.venv\Scripts\python.exe" (
  python -m venv "backend\.venv" || (echo [LOI] Tao venv that bai & pause & exit /b 1)
)
"backend\.venv\Scripts\python.exe" -m pip install -r "backend\requirements.txt" || (echo [LOI] Cai Python package that bai & pause & exit /b 1)
cd frontend
call npm install || (echo [LOI] npm install that bai & pause & exit /b 1)
cd ..
echo [OK] Da cai dependencies. Chay KhoiDong_Windows.bat
pause
