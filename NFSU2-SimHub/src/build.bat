@echo off
rem Builds NFSU2Telemetry.asi (32-bit) with Visual Studio 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS telemetry.cpp ffb.cpp /LD /Fe:NFSU2Telemetry.asi /link user32.lib
