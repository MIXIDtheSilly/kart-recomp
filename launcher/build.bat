@echo off
rem Builds out\launcher.exe: with clang from llvm-mingw (tools\llvm-mingw) when it is there or Visual Studio is not
rem (tools\toolchain.py downloads it first, 190 MB: no Visual Studio needed), else with Visual Studio's cl.
setlocal
cd /d %~dp0
if not exist out mkdir out
where python >nul 2>nul || (echo Python not found: install Python 3.13 from python.org, with "Add to PATH" ticked & exit /b 1)
for /f "usebackq delims=" %%L in (`python ..\tools\paths.py bat`) do set "%%L"
if /i "%BUILD_CC%"=="mingw" goto mingw
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  python ..\tools\toolchain.py install || exit /b 1
  goto mingw
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /utf-8 /W3 /permissive- launcher.cpp /Foout\ /Feout\launcher.exe /link /SUBSYSTEM:WINDOWS
exit /b

:mingw
echo building launcher.exe with llvm-mingw clang
"%MINGW%\bin\clang++.exe" -O2 -std=c++20 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -municode -mwindows -static launcher.cpp ^
  -o out\launcher.exe -ld2d1 -ldwrite -ldwmapi -lole32 -lshell32 -luuid
