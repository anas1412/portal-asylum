# Outlast portal gun

Portal 2's portal gun in Outlast:
- see-through oval portals, with momentum-safe teleporting for Miles and enemies
- shots that pass through gates, fences, glass and people
- the real gun model and sounds, read from your own Portal 2 install

Nothing from either game is shipped.

**Status:** native Linux Outlast (Steam build 576074, `OLGame.x86_64`). A Windows build is in progress.

## Controls
| Input | Action |
|---|---|
| Left mouse | Blue portal. Looking at a door, pickup, bed or locker? Left mouse uses it instead. |
| Right mouse | Orange portal |
| Middle mouse | Camcorder (the gun hides while it's up) |
| F | Night vision (unchanged) |

## Setup (Linux)
1. Requirements: Outlast (native Linux), Portal 2 installed (only for its files), Python 3, `uv`, `ffmpeg` and `g++`.
2. Convert the gun and sounds from your Portal 2 install. This writes `cache/`:
   ```
   uv run --with pillow python tools/p2gun.py
   ```
3. Build:
   ```
   ./build.sh
   ```
4. Steam → Outlast → Properties → Launch Options:
   ```
   LD_PRELOAD="$LD_PRELOAD:/home/<you>/outlast-portal-gun/build/libolportal.so" %command%
   ```
   - It appends to Steam's own `LD_PRELOAD`, so the overlay keeps working.
   - To uninstall, clear the box. No game file is ever changed.

## How it works
- `src/loader.cpp` (LD_PRELOAD) hooks `UOLEngine::Tick` and `PlayerInput::InputKey` through their vtable slots, plus Outlast's SDL audio callback and the buffer swap.
  - It loads `build/libolportal_mod.so`; touch `run/reload` to hot-reload it.
- `src/mod.cpp` drives the game through UE3's own reflection (`ue.h`: properties and functions by name, `ProcessEvent`).
  - **Portals:** `DynamicSMActor_Spawnable` actors. The engine sphere mesh is rebuilt at runtime into a flat disc (`mesh.h`).
  - **See-through views:** a `SceneCapture2DComponent` per portal, with an off-axis projection whose window is the exit portal.
  - **The gun:** the engine cube mesh rebuilt with Portal 2's `v_portalgun`. It uses the camcorder's lit material and is attached to Miles's camera bone.
- Dev loop: `./dev-run.sh` (windowed), commands appended to `run/cmd`, log in `run/mod.log`. `tools/drive.py` scripts the tests.
- Engine offsets and gotchas are in `MODLOG.md`.

## Credits
- Portal and Portal 2 are © Valve. Outlast is © Red Barrels.
- This mod uses their files only from your own installs.
- Built with an AI coding agent (Claude), using the [universal-modder](https://github.com/rehan-remade/universal-modder) method.
