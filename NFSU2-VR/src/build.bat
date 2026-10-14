@echo off
rem Builds NFSU2HeadTrack.asi (32-bit) with Visual Studio 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /I..\third_party\openvr /I..\third_party\vulkan headtrack.cpp /LD /Fe:NFSU2HeadTrack.asi /link user32.lib

