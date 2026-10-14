# NFSU2 SimHub telemetry, force feedback and wheel input

Makes Need for Speed Underground 2 (SPEED2.EXE v1.2) a regular game in SimHub, using SimHub's
built-in "External sims" support (no SimHub plugin needed). The same ASI also drives a force
feedback wheel and feeds wheel and pedals straight into the game.

## Install

Copy the contents of `install/` into the game folder (ASI Loader required). That puts these in `scripts/`:

- `NFSU2Telemetry.asi`
- `NFSU2Telemetry.simdef` (the game definition SimHub reads)
- `NFSU2Telemetry.ini` (SimHub host/port, rev limit, force feedback and input settings)

On every game start the ASI writes
`%LOCALAPPDATA%\SimHub\ExternalSims\Registrations\4e465355-3200-4d0d-9f1e-7e1e00000002.simlink`
pointing at the `.simdef`, and SimHub adds "Need for Speed Underground 2" to its game list.
Telemetry goes over UDP to 127.0.0.1:30777. A log is written to `scripts\NFSU2Telemetry.log`.

The settings can also be changed with the NFSU2 Mod Settings app (`../NFSU2-Settings`).

## Data

Speed, rpm, max rpm (highest seen in the race unless `MaxRpm` is set), gear, race position,
completed/total laps, current/last/best lap time, yaw/pitch/roll and their rates, local
surge/sway/heave acceleration and velocity (derived from the car's world velocity), world
position, plus the custom field `OpponentCount`.

Layout 1.1 adds throttle, brake, handbrake, per-wheel slip and lock (computed from wheel spin
vs. car speed, tyre radius learned while coasting), per-wheel suspension velocity, nitro
active (`PushToPassActive`) and the custom field `NitroBoost`. Handbrake and nitro offsets were
found by correlation and are the least certain.

NFSU2 has no fuel, damage, tyre temperature or wear, so those fields are not sent.

## Force feedback (`[FFB]`)

`src\ffb.cpp` drives a force feedback wheel through DirectInput (Fanatec preferred, built for
the CSL DD): self-aligning torque toward the front axle's direction of travel (countersteer in
slides, light wheel past the grip peak), speed-dependent weight, a wheelbase damper, bumps from
the front suspension and side-impact kicks. The log reports which device was taken. The mod
takes the wheel's force output exclusively, so the game's own force feedback must be off.

## Wheel and pedal input (`[INPUT]`)

NFSU2 can't select a wheel by itself, so the ASI reads steering from the force feedback wheel
and throttle/brake/handbrake from a pedal device (chosen by part of its name and axis names in
the ini) and feeds them into the game. Pedals calibrate themselves: keep them released when the
game starts, then press each fully once. `SteerScale` above 1 gives full lock with less wheel
rotation. Menus still use the keyboard.

## Rebuild

`src\build.bat` (Visual Studio 2022 Build Tools, x86). The packet struct in `telemetry.cpp`
must match the `.simdef`; if fields change, regenerate the definition and update the struct
(SimHub's External sims editor can generate the C++ struct).
