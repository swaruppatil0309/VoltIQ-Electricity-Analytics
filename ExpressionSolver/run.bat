@echo off
REM ---------------------------------------------------------------------------
REM  VoltIQ Expr - Expression Solver & Visualizer (Windows launcher)
REM  Requires: MinGW-w64 g++ on PATH (or Visual Studio cl.exe, see README)
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist bin mkdir bin

echo Compiling the C++17 expression engine...
g++ -std=c++17 -O2 -pthread -o bin\xpr.exe cpp\*.cpp
if errorlevel 1 (
  echo.
  echo Build failed. Make sure g++ ^(MinGW-w64^) is on your PATH.
  pause
  exit /b 1
)
echo   done: bin\xpr.exe

echo Starting the C++ engine on port 8003...
start "VoltIQ Expr - C++ engine" bin\xpr.exe serve 8003
timeout /t 2 >nul

echo Starting the dashboard on http://localhost:3000
cd web
set PORT=3000
set ENGINE_PORT=8003
node server.js
