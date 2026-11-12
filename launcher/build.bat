@echo off
rem Builds out\launcher.exe. Needs only Visual Studio's C++ tools (found with vswhere).
setlocal
cd /d %~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio not found: install it with "Desktop development with C++" & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio's C++ tools not found: add "Desktop development with C++" & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
if not exist out mkdir out
cl /nologo /O2 /EHsc /std:c++20 /utf-8 /W3 /permissive- launcher.cpp /Foout\ /Feout\launcher.exe /link /SUBSYSTEM:WINDOWS
