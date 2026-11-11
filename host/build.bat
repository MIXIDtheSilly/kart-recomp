@echo off
rem Builds out\kart.exe: the host runtime, the recompiled modules and Eglantine's GPU (video_core).
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
if not exist out\obj\nvcore mkdir out\obj\nvcore
python ..\tools\paths.py rsp egl.rsp out\egl.rsp || exit /b 1
set CFLAGS=/nologo /c /O2 /Zi /MD /std:c++20 /EHsc /GR- /permissive- /utf-8 /Zc:preprocessor /Zc:inline /Zc:externConstexpr /Zc:throwingNew /volatile:iso /W3 /wd4100 /wd4324 /wd4201 /MP%BUILD_MP% /D_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS @out\egl.rsp /I. /I"%SDL3%\include" /Foout\obj\ /Fdout\obj\vc.pdb
cl %CFLAGS% *.cpp services\*.cpp ..\gen\aot_modules.cpp "%EGL%\src\core\frontend\emu_window.cpp" "%EGL%\src\core\frontend\framebuffer_layout.cpp" nv\*.cpp nv\devices\*.cpp || exit /b 1
if not exist out\obj\nvn mkdir out\obj\nvn
cl %CFLAGS% /Foout\obj\nvn\ nvn\*.cpp nvn\vk\*.cpp || exit /b 1
rem nv\core has file names nv\devices uses too (nvmap.cpp).
cl %CFLAGS% /Foout\obj\nvcore\ nv\core\*.cpp || exit /b 1
rem /INCREMENTAL:NO: incremental linking pads data sections, which breaks clang PGO profile writing (BUILD_PGO=gen).
link /nologo /DEBUG /INCREMENTAL:NO /OUT:out\kart.exe /SUBSYSTEM:CONSOLE /IGNORE:4099 /NODEFAULTLIB:LIBCMT out\obj\*.obj out\obj\nvcore\*.obj out\obj\nvn\*.obj ^
  ..\build\rtld.lib ..\build\main.lib ..\build\subsdk0.lib ..\build\sdk.lib ^
  %EB%\src\video_core\video_core.lib %EB%\src\audio_core\audio_core.lib %EB%\_deps\cubeb-build\cubeb.lib %SDL3%\lib\libSDL3_static.lib %EB%\src\shader_recompiler\shader_recompiler.lib %EB%\src\video_core\gpu_logging\gpu_logging.lib ^
  %EB%\src\common\common.lib %EB%\externals\glad\glad.lib %EB%\externals\reshadefx.lib %EB%\externals\bc_decoder.lib ^
  %EB%\_deps\sirit-build\src\sirit.lib %EB%\_deps\fmt-build\fmt.lib %EB%\externals\stb.lib %EB%\_deps\lz4-build\lz4.lib ^
  %EB%\_deps\zstd-build\lib\zstd_static.lib %EB%\_deps\opus-build\opus.lib %EB%\_deps\zlib-build\zs.lib %EB%\externals\demangle.lib ^
  %EB%\_deps\boost-build\libs\context\libboost_context-vc145-mt-x64-1_92.lib ^
  %EB%\_deps\boost-build\libs\container\libboost_container-vc145-mt-x64-1_92.lib ^
  %EB%\_deps\boost-build\libs\filesystem\libboost_filesystem-vc145-mt-x64-1_92.lib ^
  %EB%\_deps\boost-build\libs\date_time\libboost_date_time-vc145-mt-x64-1_92.lib ^
  %CPM%\ffmpeg\9.0.2-1790726865-946fcce07b-windows-amd64\lib\swscale.lib %CPM%\ffmpeg\9.0.2-1790726865-946fcce07b-windows-amd64\lib\avcodec.lib ^
  %CPM%\ffmpeg\9.0.2-1790726865-946fcce07b-windows-amd64\lib\swresample.lib %CPM%\ffmpeg\9.0.2-1790726865-946fcce07b-windows-amd64\lib\avfilter.lib ^
  %CPM%\ffmpeg\9.0.2-1790726865-946fcce07b-windows-amd64\lib\avutil.lib ^
  user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib winmm.lib bcrypt.lib ntdll.lib ws2_32.lib uuid.lib version.lib setupapi.lib dbghelp.lib imm32.lib cfgmgr32.lib avrt.lib ksuser.lib %KART_LINK_EXTRA% || exit /b 1
