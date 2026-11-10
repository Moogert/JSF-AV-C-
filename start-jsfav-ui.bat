@echo off
setlocal
cd /d "%~dp0"
jsfav.exe serve
if errorlevel 1 (
  echo.
  echo The checker could not start. Review the error above.
  pause
)
