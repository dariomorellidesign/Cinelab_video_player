@echo off
setlocal
cd /d "%~dp0"
call prepare_dlss5_test.bat --no-pause
if errorlevel 1 (
  echo.
  pause
  exit /b 1
)
if exist "CineLabVideoPlayer.exe" (
  set "EXE=CineLabVideoPlayer.exe"
) else (
  set "EXE=build\Release\CineLabVideoPlayer.exe"
)
if not exist "%EXE%" exit /b 1
start "" "%EXE%" --quality auto %*
