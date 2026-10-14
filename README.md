# speed2vr

Mods for **Need for Speed: Underground 2** (PC, v1.2, `SPEED2.EXE` 4,800,512 bytes):

| Folder | What it does |
|---|---|
| [`NFSU2-VR`](NFSU2-VR) | 6DoF VR through SteamVR: head tracking from the driver's seat, stereo 3D, dashboard gauges (`NFSU2HeadTrack.asi`) |
| [`NFSU2-SimHub`](NFSU2-SimHub) | SimHub telemetry, force feedback and wheel/pedal input (`NFSU2Telemetry.asi`) |
| [`NFSU2-Settings`](NFSU2-Settings) | NFSU2 Mod Settings, a Windows app to edit both mods' ini files |

Each folder has its own README with install steps, an `install/` folder to copy into the game
folder, and the source in `src/`. The mods need an ASI loader (e.g. Ultimate ASI Loader as
`dinput8.dll`). DXVK, ReShade and game files are not included.

Tested with a Pico 4 over Virtual Desktop + SteamVR, an AMD RX 6800 XT and a Fanatec CSL DD.

See [CHANGELOG.md](CHANGELOG.md) for what changed between releases.

## License

MIT, see [LICENSE](LICENSE). Third-party headers in `NFSU2-VR/third_party` keep their own licenses.
