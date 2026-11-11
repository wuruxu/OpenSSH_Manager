@echo off
setlocal DisableDelayedExpansion
title OpenSSH Manager Setup
set "manager_ps=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "manager_ps=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%~dp0Install.ps1" goto missing
if not exist "%manager_ps%" goto missing
"%manager_ps%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Interactive
set "manager_result=%errorlevel%"
if not "%manager_result%"=="0" (
  echo Installer exited with code %manager_result%. Keep any error details before retrying.
  pause
)
exit /b %manager_result%
:missing
echo Windows PowerShell or Install.ps1 is missing. Fully extract the release ZIP first.
pause
exit /b 1
