@echo off
REM Locate and call vcvars64.bat from a VS2022 install. Sets up cl.exe + INCLUDE
REM / LIB so the Ninja generator can find the MSVC toolchain. Override with
REM VCVARS64=<path-to-vcvars64.bat> if VS is installed elsewhere.
if defined VCVARS64 (
  if exist "%VCVARS64%" ( call "%VCVARS64%" >nul && exit /b 0 )
)
set "_VSROOT=%ProgramFiles%\Microsoft Visual Studio\2022"
for %%E in (Community Professional Enterprise BuildTools) do (
  if exist "%_VSROOT%\%%E\VC\Auxiliary\Build\vcvars64.bat" (
    call "%_VSROOT%\%%E\VC\Auxiliary\Build\vcvars64.bat" >nul && exit /b 0
  )
)
echo Could not find vcvars64.bat for VS2022. Set VCVARS64 to its full path.
exit /b 1
