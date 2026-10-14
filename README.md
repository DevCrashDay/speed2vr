# speed2vr

Mods that turn **Need for Speed: Underground 2** (PC, 2004) into a seated VR and sim-rig game:

- **VR:** play from the driver's seat in a SteamVR headset, with full 6DoF head tracking, real
  stereo 3D and round gauges fixed on the dashboard.
- **Sim rig:** NFSU2 shows up as a regular game in [SimHub](https://www.simhubdash.com/), so
  dashboards, rev lights, bass shakers and motion platforms work. The same mod adds force feedback
  for a DirectInput wheel and feeds wheel and pedals straight into the game.
- **Settings app:** a small Windows program to change all of it without editing ini files.

The mods are ASI plugins that hook the game's own code, so they only work with the
**v1.2 `SPEED2.EXE` (4,800,512 bytes)**. They were developed and tested with a Pico 4 streamed over
Virtual Desktop into SteamVR, an AMD RX 6800 XT, a Fanatec CSL DD wheelbase and SIMSONN pedals.

| Folder | File | What it does |
|---|---|---|
| [`NFSU2-VR`](NFSU2-VR) | `NFSU2HeadTrack.asi` | 6DoF VR through SteamVR, stereo 3D, dashboard gauges |
| [`NFSU2-SimHub`](NFSU2-SimHub) | `NFSU2Telemetry.asi` | SimHub telemetry, force feedback, wheel and pedal input |
| [`NFSU2-Settings`](NFSU2-Settings) | `NFSU2 Mod Settings.exe` | Settings editor for both mods |

The two ASI mods are independent: use either one on its own or both together.

---

## NFSU2-VR (`NFSU2HeadTrack.asi`)

### Head tracking and camera

- **6DoF head tracking.** The headset's rotation and position (from SteamVR) are multiplied into
  the game's player camera, so you can look around and lean in the car. Rotation only is possible
  too (`PositionScale=0` / `UsePosition=0`), and head roll can be ignored (`UseRoll=0`).
- **Driver's seat camera.** In a race the camera sits at the driver's eye position whatever
  in-game camera view is selected. The seat can be moved live while driving (Ctrl + arrow keys,
  Ctrl + PageUp/PageDown) and saved with F9.
- **Recenter** with F8 (look straight ahead first). F7 turns head tracking on and off.
- **Only in races** by default (`OnlyInRace=1`), so menus stay normal. The mouse cursor is hidden
  while driving.
- **Wider field of view** (`GameFOV`, default 84, about 100° on a 16:9 picture).
- Pose prediction (`PredictionSeconds`) to reduce latency.
- Because the mod changes the camera matrix where the game sets it, everything derived from it
  (frustum culling, camera position) stays consistent.

### Stereo 3D

Two stereo modes, set with `[STEREO] Mode`:

- **Depth-based stereo (default, `Mode=1`).** Each game frame is drawn once from the middle of
  your head and split into a left and right eye view using the game's depth buffer. Both eyes
  update at the full game frame rate. Quality is set by `DepthSteps`, strength by `DepthScale`.
  The eye distance comes from the headset unless `IPD` is set.
- **Alternate eyes (`Mode=0`).** The game draws the left eye on one frame and the right eye on the
  next: exact geometry and full width per eye, but each eye gets half the frame rate.

### Showing the game in the headset

- **Real SteamVR app (`VRFrames=1`, default).** Each eye's picture is submitted to SteamVR together
  with the head pose it was drawn with, so SteamVR keeps it steady between game frames. `VRFrames=0`
  falls back to a SteamVR overlay.
- **GPU hand-over with DXVK (`Transport=0`).** When the game runs on
  [DXVK](https://github.com/doitsujin/dxvk) (Direct3D 9 on Vulkan), finished frames are copied to
  SteamVR on the graphics card, with no desktop capture. The render resolution then comes from
  `ResX`/`ResY` in the Widescreen Fix ini (e.g. 3840x2160) and no longer depends on your monitor.
  Without DXVK, or with `Transport=1`, the mod captures the desktop instead.
- **World scale.** The virtual screen size follows `GameFOV` (`AutoWidth=1`) so the world has real
  scale; `WidthScale` and the Ctrl+Shift keys fine-tune size, distance and height live. F10 hides the
  SteamVR screen. A black backdrop around the picture is the default (`BlackBackground=1`).

### Dashboard HUD

- **Rev counter** (with the current gear) and **speedometer** as round gauges fixed in 3D on the
  dashboard, drawn for both eyes with the game's own view and projection. Max rpm, redline and
  max speed are configurable (defaults 9,000 rpm, 7,500 redline, 320 km/h).
- **Live minimap** taken out of the game's HUD and shown as a third round instrument next to them.
- **Position / lap / lap time bar** above the gauges.
- Nothing follows your head: all instruments stay in the car.
- **Custom gauge faces** from PNG files in `gauge_mod/` (face and needle per gauge, needle sweep,
  scale and digital readout set in `gauges.ini`). Missing files fall back to the built-in drawn
  gauges. See [`gauge_mod/README.txt`](NFSU2-VR/install/gauge_mod/README.txt).
- With `[GAUGES] Enabled=0` a flat speed and gear panel is shown instead.

### VR requirements

- NFSU2 v1.2 and an ASI loader (e.g. Ultimate ASI Loader as `dinput8.dll`)
- [NFSU2 Widescreen Fix](https://github.com/ThirteenAG/WidescreenFixesPack)
- SteamVR and a headset (tested: Pico 4 through Virtual Desktop)
- Recommended: DXVK (32-bit `d3d9.dll`), needed for the GPU hand-over and resolutions above your desktop
- Optional: ReShade 5 or newer as an ASI (ReShade 4.x crashes on DXVK)

Full install steps, keys and tuning: [`NFSU2-VR/README.md`](NFSU2-VR/README.md) and
[`VR-HeadTracking.txt`](NFSU2-VR/install/VR-HeadTracking.txt).

---

## NFSU2-SimHub (`NFSU2Telemetry.asi`)

### SimHub telemetry

- NFSU2 becomes a regular game in SimHub through its built-in **External sims** support. No SimHub
  plugin is needed: on every game start the mod registers `NFSU2Telemetry.simdef` with SimHub, and
  "Need for Speed Underground 2" appears in SimHub's game list.
- Data is sent over UDP (default `127.0.0.1:30777`, a different PC can be set as host), one packet
  per new physics sample plus a heartbeat in menus and pause.
- **Fields sent:**
  - speed, rpm, max rpm, gear
  - race position, number of opponents, completed and total laps
  - current, last and best lap time
  - yaw, pitch and roll and their rates
  - surge, sway and heave acceleration and velocity (for motion rigs), world position
  - throttle, brake and handbrake
  - per-wheel tyre slip, wheel lock and suspension velocity
  - nitro active and nitro boost factor
- Max rpm (for rev lights) is the highest rpm seen in the current race, or a fixed value via
  `MaxRpm`.
- Handbrake and nitro values were found by recording game memory and are the least certain fields.

### Force feedback

For a DirectInput force feedback wheel, built and tuned for the **Fanatec CSL DD** (Fanatec devices
are picked first; a device can be chosen by name):

- self-aligning torque toward where the front wheels are actually travelling, which gives
  countersteer in slides
- a lighter wheel past the grip peak (understeer)
- weight that builds with speed, plus a light centering spring when stopped
- a damper on the wheelbase
- bumps from the front suspension
- kicks on side impacts

Overall strength (`Gain`, start low on a direct drive base), direction (`Invert`) and each effect
are set in `[FFB]`. The game's own force feedback has to be off.

### Wheel and pedal input

NFSU2 can't select a wheel in its own controller menu, so the mod reads steering from the force
feedback wheel and throttle, brake and (optionally) handbrake from a pedal device chosen by name
and axis, and feeds them straight into the game while racing.

- Pedals calibrate themselves: keep them released at game start, then press each fully once.
- `SteerScale` gives full lock with less wheel rotation; deadzones and steering inversion are configurable.
- Menus still use the keyboard.

Full details: [`NFSU2-SimHub/README.md`](NFSU2-SimHub/README.md).

---

## NFSU2-Settings (`NFSU2 Mod Settings.exe`)

A Windows app that edits `NFSU2HeadTrack.ini` and `NFSU2Telemetry.ini`:

- Tabs for VR, VR screen, VR HUD & gauges, force feedback, wheel & pedals and SimHub.
- Every key in the ini files is shown, with the ini comment as its description, so settings added
  by a later mod version show up automatically (unknown sections go on an "Other" tab).
- On/off settings are checkboxes, choices are lists, key bindings are set by pressing the key.
- Saving only replaces values; comments and layout stay. A `.bak` copy is kept.
- Changes apply on the next game start. Needs .NET Framework 4 (included in Windows 10 and 11).

Details: [`NFSU2-Settings/README.md`](NFSU2-Settings/README.md).

---

## Install overview

1. Install an ASI loader in the game folder (e.g. Ultimate ASI Loader as `dinput8.dll`).
2. Copy the `install/` folder contents of each mod you want into the game folder
   (`scripts/`, and `gauge_mod/` for the VR mod; the settings app goes next to `SPEED2.EXE`).
3. For VR: install the Widescreen Fix, optionally DXVK (+ `optional/dxvk.conf`), set the render
   resolution in `NFSUnderground2.WidescreenFix.ini`, start SteamVR first, then the game, and press
   F8 in a race.
4. For SimHub: start SimHub and pick "Need for Speed Underground 2". Turn the game's own force
   feedback off.

Each mod writes a log next to its ASI in `scripts/` (`NFSU2HeadTrack.log`, `NFSU2Telemetry.log`).
To uninstall, delete the mod's files from `scripts/` (and `d3d9.dll` / `dxvk.conf` if you added DXVK).

DXVK, ReShade, the ASI loader and game files are **not** included.

## Known limits

- Only the v1.2 `SPEED2.EXE` (4,800,512 bytes) is supported; both mods disable themselves on any other version.
- NFSU2 cars have no modelled interiors. The camera sits where the driver's eyes would be and the
  gauges are drawn by the mod, but there is no steering wheel or cockpit model.
- Depth-based stereo gives each eye half the frame width, and edges of near objects can smear a
  little. Alternate-eye mode avoids that but halves each eye's frame rate.
- The game itself renders roughly 80 to 100 fps in races (its engine is single-threaded), which
  limits the VR frame rate.
- Menus are shown as a flat screen in both eyes.
- NFSU2 has no fuel, damage, tyre temperature or tyre wear, so SimHub gets none of those.
- Wheel and pedals only work in races; menus use the keyboard.
- Frame limiters and overlays that hook the game (e.g. a frame pacer) can cap it at 60 fps or crash
  it on DXVK. AnimationController.asi occasionally crashes at game start; start again if so.

## Building

Each mod's `src/build.bat` builds it with Visual Studio 2022 Build Tools (32-bit / x86 for the
ASIs). `NFSU2-VR/third_party` holds the OpenVR and Vulkan headers.

See [CHANGELOG.md](CHANGELOG.md) for what changed between releases.

## Credits

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) and
  [Widescreen Fix](https://github.com/ThirteenAG/WidescreenFixesPack) by ThirteenAG
- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors
- [ReShade](https://reshade.me) by crosire
- [OpenVR](https://github.com/ValveSoftware/openvr) by Valve, Vulkan headers by Khronos
- [SimHub](https://www.simhubdash.com/) by Wotever

## License

MIT, see [LICENSE](LICENSE). Third-party headers in `NFSU2-VR/third_party` keep their own licenses
(OpenVR: BSD-3-Clause, Vulkan: Apache-2.0 / MIT).
