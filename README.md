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

## 1.8.0: settings in the Tune Car > Engine tab
Exhaust / intake sound type (0 = as installed, 1-4 = Stock/Street/Sport/Race, per car), exhaust and intake intensity (0-200), rasp (0-35), pipe resonance (0-200), extra pops (0-200). Only "Enabled" and "Effect on every car" stay on the Mods page.

## 1.8.1
Every Engine-tab setting (sound types, intensities, rasp, pipe resonance, pops) is saved per car (ini section [Cars], key = garage id).

## 1.8.2
Load timing: after every loading screen (HUD silent for over 3 s) one log line says how long it was and how much time our hooks spent inside the game's own function and in our own code (per hook, with the longest single call). To trace a slow load.

## 1.9.0: turbo / boost sounds
The turbo layers (spool, whistle, flutter: the samples of the sub-events driven by the Boost parameter) are recognised separately (bank_tiers.inc has a third mask; tools/tiers_table.py generates it) and get their own effect per level: brighter, louder, with a soft long echo so they float. Engine tab: "Turbo / boost sound (%)" 0-200, per car. The level follows the higher of intake / exhaust for now; the log line "car part ids from Car+0x70E0" is for finding the forced induction slot.

## 1.9.1
Turbo / boost layers: airier instead of echoing (the top end opens with the level, no delay).

## 1.9.2
Turbo layers: much stronger lift (+4.5 / +7 / +10 dB at Race), the 60 s diagnostics line counts the turbo samples played.

## 1.10.0
Turbo level = the installed forced induction part (PartEnum 24-28: single / twin / quad turbo, centrifugal / positive displacement supercharger; the car's part table starts at Car+0x70E0 for enum 2). Intake air hiss: band-limited noise (1.5-7 kHz) on the engine-bay tracks, growing with throttle x revs and the forced induction level; Engine tab row "Intake air hiss (%)", 0-200, per car. Classic (non-granular) cars only.

## 1.11.0
The turbo layers are split in two: spool / whistle / whine (events with Boost + EngineRPM) and the short blow-off / flutter one-shots (Boost only). Engine tab, per car: Turbo / supercharger volume (%), Blow-off valve volume (%, 0 = off), Blow-off valve sound (0-15: always the same one of the car's blow-off samples), Intake air hiss (%, turbos only, louder now). Volume is a plain gain inside our own effect, so it works on every level.

### 1.11.1
- Intake air hiss now follows the turbo's boost pressure (not throttle x revs).
- Diagnostic: logs how the game creates bank samples ("bank sample made: ...") - groundwork for choosing other cars' blow-off sounds.

### 1.11.2
- Own synthesised turbo whistle (rises with boost, per level) and supercharger whine (rises with revs), controlled by the turbo/supercharger volume slider; they no longer depend on the game's turbo samples.
- Air hiss follows boost pressure, also on factory turbos (quiet at stock, stronger per level); boost range is learned from the car.
- Log line "boost ..." every 5 s while boosting.

### 1.11.3
- The synthesised whistle is switched off: the turbo/supercharger sound is the game's own for the fitted part.
- Turbo and blow-off volume now follow the sliders live (before, a looping sound only picked the volume up when it started).
- Log: "turbo sample played" / "turbo track set up" lines for diagnosis.

### 1.11.4
- Turbo/blow-off: no tone change any more (100% = the game's own level), volume 200% about 3x; the turbo's loudness follows the boost pressure.

### 1.11.5
- The turbo's loudness no longer follows boost pressure (the game's own curve only); the slider scales it.

### 1.11.6
- Turbo volume follows the boost pressure again (like the hiss); slider range wider (200% about 6x at full boost); blow-off 200% likewise.
- Log line "settings of garage car N: ..." whenever the car whose settings are in force changes.
