# FH3 Let 'Em Roar – upgrade sound mod

A mod for the [FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform) (DLL: `EngineSoundTiers.dll`) that backports Forza Horizon 6's
behaviour to Forza Horizon 3: the engine's **exhaust and intake sound change with the installed upgrade level** (Stock / Street / Sport / Race),
on every car.

- Exhaust: Street = sport rear muffler, Sport = rear + middle muffler, Race = open system.
- Intake: Street = panel filter, Sport = cone filter, Race = ITB / mesh.
- Each level has its own character (not just "more of the same"): Street a bit fuller and less muffled, Sport deeper and barking, Race deep, meaty and raw.
- Nothing of the game's files is changed and no sample is replaced: FMOD's own EQ / distortion are put on the exhaust and engine-bay tracks.

## How it works
- `tools/tiers_table.py` reads the car sound banks (`PG_*`, `GS_*`) and writes `src/bank_tiers.inc`: which sample is exhaust, which is engine bay.
- The mod hooks `System::createSound` (sample -> bank + index), the Studio sound-instrument player (effect on the track's channel group),
  `System::playSound` (granular engine synth, PCM callback sounds) and the HUD controller (the driven car's exhaust/intake level, `Car+0x7120/0x7124`).
- Settings (Home > MODS > Installed mods): on/off, *Sound level* preview (the car's own parts / Stock / Street / Sport / Race), exhaust and intake effect amount, effect on every car.

Status: classic cars work; the 50 granular cars (GS_ banks) are being worked on. See `docs/`.

## Build
llvm-mingw, CMake + Ninja, with the platform checkout as `./platform` (only `include/fh3mod.h` is needed):
```
cmake -S . -B build -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++
cmake --build build
```
Output: `build/EngineSoundTiers.dll`, to be put in `fh3-mods`.
