@echo off
setlocal
cd /d "%~dp0"
if exist "CineLabVideoPlayer.exe" (
  set "EXE=CineLabVideoPlayer.exe"
) else (
  set "EXE=build\Release\CineLabVideoPlayer.exe"
)
if not exist "%EXE%" (
  echo Build first with build_windows.bat
  pause
  exit /b 1
)
"%EXE%" --output 3840x2160 --quality quality %*
