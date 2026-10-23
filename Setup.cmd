@echo off
rem Star Wars Episode I: Racer recomp: builds the game from your own ROM. See README.md, Building.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
set RC=%ERRORLEVEL%
if not defined RECOMP_LAUNCHER pause
exit /b %RC%
