# NFSU2 Mod Settings

A small Windows app for changing the settings of the VR mod (`NFSU2HeadTrack.ini`) and the
SimHub / force feedback / wheel input mod (`NFSU2Telemetry.ini`) without editing the ini files by hand.

- Every key in the ini files is shown, so new settings appear automatically.
- The comment next to a key becomes its description.
- Saving only replaces the values; comments and layout stay as they are.

## Install

Copy `install/NFSU2 Mod Settings.exe` into the game folder (next to `SPEED2.EXE`) and run it.
It reads and writes the ini files in `.\scripts`. Changes apply on the next game start.

Needs .NET Framework 4 (included in Windows 10 and 11).

## Building

Install Visual Studio 2022 Build Tools, then run `src/build.bat`. The output is
`src/NFSU2 Mod Settings.exe`.
