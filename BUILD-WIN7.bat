@echo off
title Build FLEX Download Manager - Windows 7
echo =========================================================
echo   Building FLEX Download Manager for Windows 7
echo =========================================================
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
echo Done! Output in build\FLEXDownloadManager.exe
pause
