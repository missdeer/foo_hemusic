@echo off
REM Configure foo_hemusic with the Ninja Multi-Config generator under the MSVC
REM developer environment (Ninja needs cl.exe on PATH; the VS generator finds
REM it automatically, Ninja does not). Generates build/compile_commands.json.
setlocal
call "%~dp0_vcvars.bat" || exit /b 1
cd /d "%~dp0.."
REM x64-windows-static-md = static Boost/Catch2 libs + dynamic CRT (/MD), so the
REM component links Boost.JSON statically (no boost_json.dll to ship) while still
REM agreeing with foobar2000's shared.dll on the /MD runtime.
cmake -S . -B build -G "Ninja Multi-Config" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake %*
