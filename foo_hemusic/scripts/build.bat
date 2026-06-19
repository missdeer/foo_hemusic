@echo off
REM Build foo_hemusic under the MSVC developer environment.
REM Usage: scripts\build.bat [Release|Debug]   (default: Release)
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
call "%~dp0_vcvars.bat" || exit /b 1
cd /d "%~dp0.."
cmake --build build --config %CONFIG%
