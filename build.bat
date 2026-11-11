@echo off
rem SimplePic direct build script (MSVC cl.exe, no CMake needed)
cd /d "%~dp0"

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

call %VCVARS% || exit /b 1

set CFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Ithird_party /Isrc

cl %CFLAGS% src\main.cpp src\codec\codec.cpp src\codec\jpeg_meta.cpp src\ops\ops.cpp src\pipeline\pipeline.cpp src\ui\main_window.cpp ^
  /Fe:SimplePic.exe /Fo:build\ ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib shlwapi.lib || exit /b 1

mt.exe -nologo -manifest src\app.manifest -outputresource:SimplePic.exe;1 || exit /b 1

echo BUILD OK: SimplePic.exe
dir SimplePic.exe | findstr SimplePic
