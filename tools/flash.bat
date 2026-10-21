@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
  where python >nul 2>nul && set "PY=python"
)
if not defined PY (
  echo.
  echo Python 3 was not found.
  echo Install it from https://www.python.org/downloads/windows/
  echo and check "Add python.exe to PATH", then run this file again.
  echo.
  pause
  exit /b 1
)

%PY% "%~dp0flash.py" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo Flash failed. Send the text above to the seller.
pause
exit /b %RC%
