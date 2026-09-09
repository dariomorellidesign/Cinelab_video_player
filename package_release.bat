@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set /p VERSION=<VERSION
rem Step07D is the validated public-player build. The promotion script stages
rem only official, production-signed NVIDIA Streamline components for FG.
set "SRC=build\Release"
set "STAGE=dist\CineLabVideoPlayer-v%VERSION%-win64"
set "ZIP=dist\CineLabVideoPlayer-v%VERSION%-win64.zip"

if not exist "%SRC%\CineLabVideoPlayer.exe" (
  echo [ERROR] Promote the validated player first with tools\Promote-Step07DToRelease.ps1
  exit /b 1
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if not exist "dist" mkdir "dist"
mkdir "%STAGE%"

copy /y "%SRC%\CineLabVideoPlayer.exe" "%STAGE%\CineLabVideoPlayer.exe" >nul
copy /y "%SRC%\ffmpeg.exe" "%STAGE%\ffmpeg.exe" >nul
copy /y "%SRC%\ffprobe.exe" "%STAGE%\ffprobe.exe" >nul
copy /y "%SRC%\sl.common.dll" "%STAGE%\sl.common.dll" >nul
copy /y "%SRC%\sl.interposer.dll" "%STAGE%\sl.interposer.dll" >nul
copy /y "%SRC%\sl.dlss.dll" "%STAGE%\sl.dlss.dll" >nul
copy /y "%SRC%\sl.dlss_g.dll" "%STAGE%\sl.dlss_g.dll" >nul
copy /y "%SRC%\sl.reflex.dll" "%STAGE%\sl.reflex.dll" >nul
copy /y "%SRC%\sl.pcl.dll" "%STAGE%\sl.pcl.dll" >nul
copy /y "%SRC%\nvngx_dlssg.dll" "%STAGE%\nvngx_dlssg.dll" >nul
copy /y "%SRC%\nvngx_dlss.license.txt" "%STAGE%\nvngx_dlss.license.txt" >nul
copy /y "%SRC%\reflex.license.txt" "%STAGE%\reflex.license.txt" >nul
copy /y "%SRC%\NVIDIA_STREAMLINE_LICENSE.txt" "%STAGE%\NVIDIA_STREAMLINE_LICENSE.txt" >nul

rem Always package the official DLSS SR DLL, not a locally staged experimental replacement.
if exist "external\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll" (
  copy /y "external\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll" "%STAGE%\nvngx_dlss.dll" >nul
) else (
  copy /y "%SRC%\nvngx_dlss.dll" "%STAGE%\nvngx_dlss.dll" >nul
)

xcopy /e /i /y "languages\*" "%STAGE%\languages\" >nul
xcopy /e /i /y "docs\*" "%STAGE%\docs\" >nul
copy /y "README.md" "%STAGE%\README.md" >nul
copy /y "LICENSE" "%STAGE%\LICENSE" >nul
copy /y "THIRD_PARTY.md" "%STAGE%\THIRD_PARTY.md" >nul
copy /y "CHANGELOG.md" "%STAGE%\CHANGELOG.md" >nul

if exist "%ZIP%" del /f /q "%ZIP%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -CompressionLevel Optimal"
if errorlevel 1 exit /b 1

echo [OK] %ZIP%
echo Official NVIDIA Streamline DLSS-G runtime included. Optional RenoDX / NR / ReShade files are not bundled.
