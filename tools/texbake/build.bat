@echo off
rem Builds texbake.exe (offline ASTC -> BC tool, see texbake.cpp) next to this script.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
cl /nologo /O2 /MD /std:c++20 /EHsc /permissive- /utf-8 /W3 texbake.cpp /Fe:texbake.exe /Fo:texbake.obj || exit /b 1
