@echo off
rem Builds "NFSU2 Mod Settings.exe" (.NET Framework 4, no extra runtime needed).
rem The game's NFSU_icon.ico is used as the program icon when this folder sits two levels below the game folder.
cd /d "%~dp0"
set FW=C:\Windows\Microsoft.NET\Framework\v4.0.30319
set ICON=
if exist "..\..\NFSU_icon.ico" set ICON=/win32icon:"..\..\NFSU_icon.ico"
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /target:winexe /optimize+ /out:"NFSU2 Mod Settings.exe" %ICON% /r:"%FW%\System.dll" /r:"%FW%\System.Core.dll" /r:"%FW%\System.Drawing.dll" /r:"%FW%\System.Windows.Forms.dll" SettingsApp.cs
