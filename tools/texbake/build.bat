@echo off
rem Builds texbake.exe (offline ASTC -> BC tool, see texbake.cpp) next to this script.
cd /d %~dp0
for /f "usebackq delims=" %%L in (`python ..\paths.py bat`) do set "%%L"
call "%VCVARS%" >nul 2>nul
cl /nologo /O2 /MD /std:c++20 /EHsc /permissive- /utf-8 /W3 texbake.cpp /Fe:texbake.exe /Fo:texbake.obj || exit /b 1
