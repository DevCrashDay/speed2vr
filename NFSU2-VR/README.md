# NFSU2 VR

A mod that adds 6DoF VR to **Need for Speed: Underground 2** (PC, v1.2) through SteamVR:

- Your headset's rotation and position drive the in-game camera from the driver's seat.
- Real stereo 3D from the game's depth buffer, so both eyes update at the full game frame rate.
- Round gauges for revs and speed, plus the minimap and a position/lap/time bar, all fixed on the dashboard.
- Custom gauge faces from PNG files in the `gauge_mod` folder.
- With DXVK, frames go to SteamVR directly on the GPU. The render resolution (e.g. 3840x2160) is then independent of your monitor.

Tested with a Pico 4 over Virtual Desktop + SteamVR, on an AMD RX 6800 XT.

## Requirements

- NFSU2 **v1.2** (`SPEED2.EXE`, 4,800,512 bytes)
- An ASI loader (e.g. Ultimate ASI Loader as `dinput8.dll`)
- [NFSU2 Widescreen Fix](https://github.com/ThirteenAG/WidescreenFixesPack)
- SteamVR
- Recommended: [DXVK](https://github.com/doitsujin/dxvk/releases) (32-bit `d3d9.dll`). It is needed for the GPU hand-over and for resolutions above your desktop.
- Optional: [ReShade](https://reshade.me) **5 or newer**, installed as an ASI. ReShade 4.x crashes on DXVK.

## Install

1. Copy the contents of `install/` into the game folder (`scripts/` and `gauge_mod/`).
2. Recommended: copy `x32/d3d9.dll` from the DXVK release into the game folder, plus `optional/dxvk.conf`.
3. In `scripts/NFSUnderground2.WidescreenFix.ini`, set `ResX`/`ResY` to the render resolution you want (e.g. 3840 / 2160).
   `optional/scripts/` contains the Widescreen Fix files used during testing. That `.dat` moves the stock HUD out of the way of the dashboard gauges.
4. Start SteamVR, then start the game. In a race, look straight ahead and press **F8** to recenter.

Keys, settings and tuning: see `install/VR-HeadTracking.txt` and the comments in `NFSU2HeadTrack.ini`.

To uninstall, delete `scripts/NFSU2HeadTrack.*` (and `d3d9.dll` / `dxvk.conf` if you added DXVK).

## Building

Install Visual Studio 2022 Build Tools with the C++ workload, then run `src/build.bat`. The output is `src/NFSU2HeadTrack.asi` (32-bit).
`third_party/` contains the OpenVR header (Valve, BSD-3-Clause) and the Vulkan headers (Khronos, Apache-2.0 / MIT). Their licenses are in the file headers.

## Known issues

- AnimationController.asi occasionally crashes at game start. If that happens, start the game again.
- Tools that hook the game's frame rate (frame limiters, overlays) can cap it at 60 fps or crash it on DXVK. Close them.

## Credits

DXVK by Philip Rebohle and contributors, Widescreen Fix by ThirteenAG, ReShade by crosire, OpenVR by Valve.
