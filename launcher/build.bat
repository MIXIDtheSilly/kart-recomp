@echo off
rem Builds out\launcher.exe with clang from llvm-mingw (tools\llvm-mingw; tools\toolchain.py downloads it first when it
rem is missing, 190 MB: no Visual Studio needed), or with Visual Studio's cl when BUILD_CC=cl or clang asks for it.
rem Needs nothing installed: Python comes into tools\python when the machine has none.
setlocal
cd /d %~dp0
if not exist out mkdir out
rem Python 3.13+: the machine's, else tools\python (tools\bootstrap.ps1 installs it there, 15 MB, no admin rights). Run,
rem not looked up: Windows has a python.exe stub that only opens the Store.
if exist ..\tools\python\python.exe set "PATH=%~dp0..\tools\python;%~dp0..\tools\python\Scripts;%PATH%"
python -c "import sys; sys.exit(sys.version_info < (3, 13))" >nul 2>nul || (
  echo No Python 3.13 or newer: installing it into tools\python
  powershell -NoProfile -ExecutionPolicy Bypass -File ..\tools\bootstrap.ps1 python || exit /b 1
  set "PATH=%~dp0..\tools\python;%~dp0..\tools\python\Scripts;%PATH%"
)
for /f "usebackq delims=" %%L in (`python ..\tools\paths.py bat`) do set "%%L"
if /i not "%BUILD_CC%"=="cl" if /i not "%BUILD_CC%"=="clang" if /i not "%BUILD_CC%"=="clang-cl" goto mingw
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
if not exist "%MINGW%\bin\clang++.exe" (python ..\tools\toolchain.py install || exit /b 1)
echo building launcher.exe with llvm-mingw clang
"%MINGW%\bin\clang++.exe" -O2 -std=c++20 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -municode -mwindows -static launcher.cpp ^
  -o out\launcher.exe -ld2d1 -ldwrite -ldwmapi -lole32 -lshell32 -luuid
