# CWR Radar HMMWV

This local addon adds `CWR_RadarHMMWV`, a radar-equipped HMMWV carrying
CAS TaticMissiles in two AH-64-inspired side banks.

- The vehicle starts with both `CAS_TaticMissile` and `CAS_TaticMissileB`.
  B inherits the original missile unchanged except for a 0.5 m indirect-damage
  radius (the original's radius is 32 m).
- The Action menu switches the complete launcher bank between 45-degree
  oblique and 90-degree vertical positions.
- The external launcher packs, launch origin, and launch direction share the
  same named state, so the selected elevation changes both the model and the
  trajectory without altering the original HMMWV mesh or textures.
- Its tank-style unit display exposes the radar; select `CAS_TaticMissile` and
  press Tab to cycle lockable armoured and infantry targets. Its dedicated
  12 km radar is all-aspect and ignores terrain, buildings, fog, and vehicle
  attitude for contact and lock selection; missile flight itself still
  collides with the world.
- The driver Action menu has **Track Cam: on/off**. When enabled, either
  missile launch uses the game's native third-person view to follow the
  missile. Normal third-person mouse controls and scenery clipping apply; the
  camera mode and time multiplier active before launch are restored when the
  missile is destroyed. Track Cam runs at 0.3x speed.
- The editor lists it under West / Armored as **HMMWV Radar / TaticMissile**.

Build the addon with `./build-mod.ps1`, then launch the included firing-range
mission with `./run-demo.ps1`. The mission puts the player in the driver seat
with two powered East armored targets ahead for the Tab-lock demonstration.
