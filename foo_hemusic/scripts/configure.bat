@echo off
REM Configure foo_hemusic with the Ninja Multi-Config generator under the MSVC
REM developer environment (Ninja needs cl.exe on PATH; the VS generator finds
REM it automatically, Ninja does not). Generates build/compile_commands.json.
setlocal
call "%~dp0_vcvars.bat" || exit /b 1
cd /d "%~dp0.."
cmake -S . -B build -G "Ninja Multi-Config" ^
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake %*
