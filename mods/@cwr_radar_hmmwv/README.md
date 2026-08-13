# CWR Radar HMMWV

This local addon adds `CWR_RadarHMMWV`, a radar-equipped HMMWV carrying eight
Maverick missiles in two AH-64-inspired side banks.

- The vehicle starts with `MaverickLauncher` and eight rounds.
- The Action menu switches the complete launcher bank between 45-degree
  oblique and 90-degree vertical positions.
- Missile proxies, launch origin, and launch direction share the same animation,
  so the selected elevation changes both the model and the trajectory.
- The editor lists it under West / Armored as **HMMWV Radar / Maverick**.

Build the addon with `./build-mod.ps1`, then launch the included firing-range
mission with `./run-demo.ps1`. The mission puts the player in the driver seat
with two empty armored targets ahead.
