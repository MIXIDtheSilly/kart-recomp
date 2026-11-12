@echo off
rem Builds out\kart.exe: the host runtime, the recompiled modules and the native NVN renderer.
rem Everything compiles with Eglantine's core flags (egl.rsp, written to out\egl.rsp with the paths filled in:
rem defines and include paths, no RTTI) so class layouts match the prebuilt Eglantine libraries.
rem Paths (Eglantine, its build, Visual Studio) come from tools\paths.py: the environment, local.cfg, or deps\.
rem BUILD_MP=<n>: at most n compiler processes (default: one per core).
cd /d %~dp0
for /f "usebackq delims=" %%L in (`python ..\tools\paths.py bat`) do set "%%L"
if not exist "%EGL%\src\video_core" (echo Eglantine not found at "%EGL%": run python tools\setup_deps.py, or set EGL in local.cfg & exit /b 1)
if not exist "%EGL_BUILD%\src\video_core\video_core.lib" (echo Eglantine's libraries not found in "%EGL_BUILD%": run python tools\setup_deps.py egl & exit /b 1)
call "%VCVARS%" >nul 2>nul
set EB=%EGL_BUILD%
set CPM=%EGL%\.cache\cpm
set SDL3=%CPM%\sdl3\3.4.14-1788231389-147a8ee32d-windows-amd64
if not exist out\obj mkdir out\obj
if not exist out\obj\nvn mkdir out\obj\nvn
rem Every file compiles again each build: objects of files that are gone must not link
del /q out\obj\*.obj out\obj\nvn\*.obj 2>nul
if exist out\obj\nvcore rd /s /q out\obj\nvcore
if exist out\obj\audio_core rd /s /q out\obj\audio_core
python ..\tools\paths.py rsp egl.rsp out\egl.rsp || exit /b 1
rem /I. comes first: audio_core\ is the host's own copy, not Eglantine's.
set CFLAGS=/nologo /c /O2 /Zi /MD /std:c++20 /EHsc /GR- /permissive- /utf-8 /Zc:preprocessor /Zc:inline /Zc:externConstexpr /Zc:throwingNew /volatile:iso /W3 /wd4100 /wd4324 /wd4201 /MP%BUILD_MP% /D_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS /I. @out\egl.rsp /I"%SDL3%\include" /Foout\obj\ /Fdout\obj\vc.pdb
cl %CFLAGS% *.cpp services\*.cpp "%ART%\gen\aot_modules.cpp" || exit /b 1
cl %CFLAGS% /Foout\obj\nvn\ nvn\*.cpp nvn\vk\*.cpp || exit /b 1
rem The audio renderer (audio_core\, adapted from Eden) builds without Eglantine's flags. Its folders compile
rem one by one: several hold files of the same name.
set ACFLAGS=/nologo /c /O2 /Zi /MD /std:c++20 /EHsc /permissive- /utf-8 /Zc:preprocessor /W3 /wd4100 /wd4324 /wd4201 /MP%BUILD_MP% /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I. /Fdout\obj\audio.pdb
setlocal enabledelayedexpansion
for /r audio_core %%D in (.) do (
  set "AD=%%~fD"
  set "AD=!AD:%CD%\=!"
  if exist "!AD!\*.cpp" (
    if not exist "out\obj\!AD!" mkdir "out\obj\!AD!"
    cl %ACFLAGS% /Foout\obj\!AD!\ "!AD!\*.cpp" || exit /b 1
  )
)
endlocal
dir /s /b out\obj\audio_core\*.obj > out\audio_objs.rsp
rem Still from Eglantine's build: video_core.lib for the ASTC decoder and BC1/BC3 encoder only (stb does the
rem block encoding), common.lib, SDL3 (gamepads), fmt, lz4, zstd, opus.
rem /INCREMENTAL:NO: incremental linking pads data sections, which breaks clang PGO profile writing (BUILD_PGO=gen).
link /nologo /DEBUG /INCREMENTAL:NO /OUT:out\%KART_EXE% /SUBSYSTEM:CONSOLE /IGNORE:4099 /NODEFAULTLIB:LIBCMT out\obj\*.obj out\obj\nvn\*.obj @out\audio_objs.rsp ^
  @"%ART%\gen\aot_libs.rsp" ^
  %EB%\src\video_core\video_core.lib %SDL3%\lib\libSDL3_static.lib %EB%\src\common\common.lib %EB%\externals\stb.lib ^
  %EB%\_deps\fmt-build\fmt.lib %EB%\_deps\lz4-build\lz4.lib %EB%\_deps\zstd-build\lib\zstd_static.lib %EB%\_deps\opus-build\opus.lib ^
  user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib winmm.lib bcrypt.lib ntdll.lib ws2_32.lib uuid.lib version.lib setupapi.lib dbghelp.lib imm32.lib cfgmgr32.lib avrt.lib ksuser.lib %KART_LINK_EXTRA% || exit /b 1
