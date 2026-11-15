@echo off
REM ---------------------------------------------------------------------------
REM  VoltIQ Care - Smart Hospital Management System (Windows launcher)
REM  Requires: MinGW-w64 g++ on PATH (or Visual Studio's cl.exe, see README)
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist bin mkdir bin

echo Compiling the C++17 DSA engine...
g++ -std=c++17 -O2 -pthread -o bin\hospital.exe cpp\*.cpp
if errorlevel 1 (
  echo.
  echo Build failed. Make sure g++ ^(MinGW-w64^) is on your PATH.
  pause
  exit /b 1
)
echo   done: bin\hospital.exe

echo Starting the C++ engine on port 8001...
start "VoltIQ Care - C++ engine" bin\hospital.exe serve 8001
timeout /t 2 >nul

echo Starting the dashboard on http://localhost:3000
cd web
set PORT=3000
set ENGINE_PORT=8001
node server.js
