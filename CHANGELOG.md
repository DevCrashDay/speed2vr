# Changelog

## v0.2 (2026-10-14)

- New: **NFSU2-SimHub** (`NFSU2Telemetry.asi`). NFSU2 shows up as a game in SimHub through
  External sims, with speed, rpm, gear, laps, lap times, position, motion data, pedals,
  per-wheel slip/lock and nitro.
- New: force feedback for DirectInput wheels (built for the Fanatec CSL DD): aligning torque,
  speed-dependent weight, damper, bumps and impact kicks.
- New: wheel and pedal input fed straight into the game, with self-calibrating pedals.
- New: **NFSU2 Mod Settings** app to edit the VR and SimHub/FFB/input settings without touching
  the ini files.
- Repo layout: one folder per mod, each with `install/`, `src/` and a README; one MIT license at the top.
- VR mod (`NFSU2HeadTrack.asi`) is unchanged from v0.1.

## v0.1 (2026-10-11)

- First release of the VR mod: 6DoF head tracking through SteamVR, stereo 3D from the depth
  buffer, dashboard gauges and minimap, custom gauge faces, DXVK GPU hand-over.
