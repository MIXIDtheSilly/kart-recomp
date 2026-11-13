@echo off
rem Builds out\kart.exe: the host runtime, the recompiled modules and the native NVN renderer.
rem Third-party code comes from ..\third_party (each folder with its license).
rem Paths (the compiler, the game version's module libraries) come from tools\paths.py.
rem BUILD_MP=<n>: at most n compiler processes (default: one per core).
cd /d %~dp0
for /f "usebackq delims=" %%L in (`python ..\tools\paths.py bat`) do set "%%L"
rem BUILD_CC=mingw (the default once the launcher's Set up has downloaded llvm-mingw): no Visual Studio needed,
rem tools\build_host.py builds the same exe with clang. Below: Visual Studio (BUILD_CC=cl or clang).
if /i "%BUILD_CC%"=="mingw" (
  python ..\tools\build_host.py
  exit /b
)
call "%VCVARS%" >nul 2>nul
set TP=..\third_party
if not exist out\obj mkdir out\obj
if not exist out\obj\nvn mkdir out\obj\nvn
rem Every host file compiles again each build: objects of files that are gone must not link
del /q out\obj\*.obj out\obj\nvn\*.obj 2>nul
if exist out\obj\nvcore rd /s /q out\obj\nvcore
if exist out\obj\audio_core rd /s /q out\obj\audio_core
if exist out\egl.rsp del out\egl.rsp

set DEFS=/DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
set INCS=/I. /I..\runtime /I%TP%\vulkan\include /I%TP%\vma /I%TP%\lz4 /I%TP%\zstd /I%TP%\opus\include /I%TP%\stb
set CFLAGS=/nologo /c /O2 /Zi /MD /std:c++20 /EHsc /GR- /permissive- /utf-8 /Zc:preprocessor /Zc:inline /Zc:externConstexpr /Zc:throwingNew /volatile:iso /W3 /wd4100 /wd4324 /wd4201 /MP%BUILD_MP% /D_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS %DEFS% %INCS% /Foout\obj\ /Fdout\obj\vc.pdb
cl %CFLAGS% *.cpp services\*.cpp "%ART%\gen\aot_modules.cpp" || exit /b 1
cl %CFLAGS% /Foout\obj\nvn\ nvn\*.cpp nvn\vk\*.cpp || exit /b 1

rem The audio renderer (audio_core\, adapted from Eden) builds with RTTI. Its folders compile one by one: several
rem hold files of the same name.
set ACFLAGS=/nologo /c /O2 /Zi /MD /std:c++20 /EHsc /permissive- /utf-8 /Zc:preprocessor /W3 /wd4100 /wd4324 /wd4201 /MP%BUILD_MP% %DEFS% /I. /Fdout\obj\audio.pdb
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

rem Third-party libraries: compiled once into out\obj\third_party (delete that folder after changing them).
set TFLAGS=/nologo /c /O2 /Zi /MD /W0 /MP%BUILD_MP% /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Fdout\obj\third_party.pdb
set TO=out\obj\third_party
if not exist %TO%\lz4\lz4.obj (
  if not exist %TO%\lz4 mkdir %TO%\lz4
  cl %TFLAGS% /Fo%TO%\lz4\ %TP%\lz4\lz4.c || exit /b 1
)
if not exist %TO%\zstd\zstd_decompress.obj (
  if not exist %TO%\zstd mkdir %TO%\zstd
  cl %TFLAGS% /DZSTD_DISABLE_ASM/I%TP%\zstd /Fo%TO%\zstd\ %TP%\zstd\common\*.c %TP%\zstd\decompress\*.c || exit /b 1
)
if not exist %TO%\stb\stb_dxt.obj (
  if not exist %TO%\stb mkdir %TO%\stb
  cl %TFLAGS% /I%TP%\stb /Fo%TO%\stb\ %TP%\stb\stb_dxt.cpp || exit /b 1
)
rem Opus: the float build in plain C (no SIMD paths), decoder and encoder as upstream lists them.
if not exist %TO%\opus\opus_decoder.obj (
  if not exist %TO%\opus mkdir %TO%\opus
  cl %TFLAGS% /DOPUS_BUILD /DUSE_ALLOCA /DHAVE_LRINT /DHAVE_LRINTF /DDISABLE_DEBUG_FLOAT /I%TP%\opus /I%TP%\opus\include /I%TP%\opus\celt /I%TP%\opus\silk /I%TP%\opus\silk\float /Fo%TO%\opus\ %TP%\opus\celt\*.c %TP%\opus\silk\*.c %TP%\opus\silk\float\*.c %TP%\opus\src\*.c || exit /b 1
)

rem /INCREMENTAL:NO: incremental linking pads data sections, which breaks clang PGO profile writing (BUILD_PGO=gen).
link /nologo /DEBUG /INCREMENTAL:NO /OUT:out\%KART_EXE% /SUBSYSTEM:CONSOLE /IGNORE:4099 /NODEFAULTLIB:LIBCMT out\obj\*.obj out\obj\nvn\*.obj @out\audio_objs.rsp ^
  %TO%\lz4\*.obj %TO%\zstd\*.obj %TO%\stb\*.obj %TO%\opus\*.obj @"%ART%\gen\aot_libs.rsp" ^
  user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib winmm.lib bcrypt.lib ntdll.lib ws2_32.lib uuid.lib version.lib setupapi.lib dbghelp.lib imm32.lib cfgmgr32.lib avrt.lib ksuser.lib hid.lib xinput.lib %KART_LINK_EXTRA% || exit /b 1
