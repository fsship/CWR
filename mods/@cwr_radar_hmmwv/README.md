# CWR Radar HMMWV

This local addon adds `CWR_RadarHMMWV`, a radar-equipped HMMWV carrying eight
CAS TaticMissiles in two AH-64-inspired side banks.

- The vehicle starts with `CAS_TaticMissile`.
- The Action menu switches the complete launcher bank between 45-degree
  oblique and 90-degree vertical positions.
- The external launcher packs, launch origin, and launch direction share the
  same named state, so the selected elevation changes both the model and the
  trajectory without altering the original HMMWV mesh or textures.
- Its tank-style unit display exposes the radar; select `CAS_TaticMissile` and
  press Tab to cycle lockable armoured targets.
- The editor lists it under West / Armored as **HMMWV Radar / TaticMissile**.

Build the addon with `./build-mod.ps1`, then launch the included firing-range
mission with `./run-demo.ps1`. The mission puts the player in the driver seat
with two powered East armored targets ahead for the Tab-lock demonstration.
