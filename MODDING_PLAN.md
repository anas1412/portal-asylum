# Outlast portal gun: research and plan

## What we're working with
- **Outlast**, Steam build 576074, **native Linux port**: `Binaries/Linux/OLGame.x86_64`, SDL2 + OpenGL, packages in `OLGame/CookedLinux`.
  - The engine is Unreal Engine 3: package version 882/3, engine build 12048, cooker 136, LZO-compressed packages.
  - That version is newer than any public UDK release, so UDK can't compile scripts for it.
- **The binary is not stripped and has debug info**: 148k symbols with real C++ names.
  - Examples: `UObject::ProcessEvent`, `UWorld::SpawnActor`, `UWorld::SingleLineCheck`, `AActor::SetLocation`,
    `UMaterialInstanceConstant::SetTextureParameterValue`, `UCanvas::DrawTile`, `UTexture2D::CreateResourceMem`.
  - This is the single biggest advantage: no signature scanning, no guesswork.
- The engine still has UE3's portal and scene-capture classes: `PortalTeleporter`, `SceneCapturePortalComponent`,
  `SceneCapture2DActor`, `TextureRenderTarget2D`.
- **Outlast's own content already does live camera-to-texture.**
  - Mirrors: material `Mirror-01` with texture parameter `Mirror_Tex`, fed by a `TextureRenderTarget2D`.
  - Scripted `SceneCapture2DActor` in `Center_block_sript`.
  - These shaders are already cooked, which is what makes see-through portals possible without an editor.
- `SDL_GL_SwapWindow` is a dynamic import, so an `LD_PRELOAD` library can draw an overlay and read input.
- Portal 2 is installed: the portal gun model, textures and sounds are in its VPKs. They are read on this PC only and never shipped.

## Routes considered
| Route | Verdict |
|---|---|
| OutlastSDK / UnrealScript `.u` mods (matty1406 toolkit) | Windows-only. Needs a "retail package patch", a rebuilt engine and a proprietary runtime that isn't published. **No.** |
| Hydle's console-unlock `Engine.upk` | Windows `CookedPCConsole` file. Not needed: we can call `UGameEngine::Exec` ourselves. |
| UPK bytecode patching | Possible, but painful and fragile next to a native hook with symbols. Backup route only. |
| **Native `LD_PRELOAD` .so** (C++, calling engine functions by symbol) | **Chosen.** No game files changed. Enable it with a Steam launch option; remove the option to uninstall. |

## What's possible
| Feature | Status | How |
|---|---|---|
| Fire blue/orange portals onto walls, floors and ceilings | ✅ | `SingleLineCheck` from the camera; place on the hit normal |
| Walk through and keep momentum | ✅ | Overlap test each tick; transform location, rotation and velocity (`SetLocation` + controller rotation) |
| Portal rings (blue/orange ovals) | ✅ | Textures generated at runtime (`CreateResourceMem`), drawn on a quad |
| **See through portals (live view)** | ⚠️ likely | `SceneCapture2DActor` → `TextureRenderTarget2D` → a material instance of Outlast's mirror material with `Mirror_Tex` swapped. **Risk:** that material is only loaded in Center Block maps. Fallback: another loaded material with a texture parameter (lit, so it looks dimmer), or rings without a view |
| Portal gun in Miles's hands | ✅ in-engine (plan A) | The binary still contains `UStaticMesh::Build`, `FStaticMeshVertexBuffer::Init` and `UStaticMeshComponent::SetStaticMesh`. Build a real UE3 static mesh at runtime from **your** Portal 2 `v_portalgun` geometry, with a runtime texture on an instance of an existing cooked lit material, attached to the camera. Outlast lights it and night vision applies. Animation is whole-gun kick/bob (plus prongs as separate part meshes), not the full skeletal set. Fallback: our own GL overlay pass before swap |
| Portal 2 sounds (fire, enter, exit) | ✅ | Play WAVs from your Portal 2 install through SDL audio, outside Wwise |
| Enemies fall or walk through portals | ✅ | Same teleport code applied to `OLEnemy*` / `OLBot` pawns, e.g. a portal under an enemy's feet drops them out the other one |
| Enemies *choose* to path through portals | ❌ | AI navigation doesn't know portals exist; after a teleport they re-path back to you the normal way |
| Scripted enemies (chases, cutscenes) | ⚠️ | Teleporting them mid-script may stall the sequence; reload the checkpoint. Option: exclude scripted ones |
| Portals inside portals (infinite tunnel), gels, cubes through portals | ❌ | Out of scope |
| Windows / Proton build | ❌ | The plan relies on the Linux binary's symbols |

## Plan (each step ends with a check in the real game)
1. **Hook skeleton.** Build `libolportal.so`; launch option `LD_PRELOAD=~/outlast-portal-gun/libolportal.so %command%`.
   Hook `ProcessEvent` and `SDL_GL_SwapWindow`, write a log.
   → verify: the log lists ticking `OLHero` / `OLPlayerController` events.
2. **Aim and mark.** Mouse1/Mouse2 trace from the camera and log the hit point and normal. Draw a debug crosshair.
   → verify: a screenshot shows the hit where you aim.
3. **Portals that work, no view yet.** Spawn two ring quads and teleport Miles between them with the transform maths, keeping speed and facing.
   Also teleport enemies (`OLEnemy*` / `OLBot`).
   → verify: the logged position jumps, a screenshot before and after, and an enemy dropped through a floor portal.
   *This alone is a playable portal gun.*
4. **See-through.** Two scene captures, render targets and mirror-material instances. Try the fallback material on maps outside Center Block.
   → verify: screenshots on 3 maps.
5. **Gun and sounds.** A converter reads Portal 2's VPKs on this PC (gun model and animations, portal ring and particle textures, WAVs) into a local cache, using the repo's Source MDL/VTF/VPK technique note. Build the gun as an in-engine static mesh attached to the camera (overlay fallback if `Build` doesn't work at runtime), and add sounds.
   → verify: a screenshot and a short clip.
6. **Package.** Script, README and uninstall note, with no game files. Optional universal-modder field note (PR only with your OK).

## Inventory and switching
- Outlast has no item wheel. Miles carries the camcorder (RMB raises it, F toggles night vision); batteries and documents are automatic.
- The mod adds the portal gun as a second held item. It's given automatically at game start and on every loaded save.
- **Q (or the mouse wheel) cycles: portal gun → camcorder → empty hands.**
  - Gun out: LMB = blue, RMB = orange. Outlast's RMB camcorder toggle is suppressed while the gun is out.
  - Camcorder out: vanilla controls.
- The game has leftover weapon hooks (`bHasWeaponEquipped`, `bUsingWeapon`, `AnimNameEquipWeapon` / `AnimNameUnequipWeapon` on the hero).
  - Try them first for the raise/lower animation and state; otherwise do our own lerp.
  - Swapping uses the vanilla camcorder raise/lower so its HUD and battery stay consistent.
- A step 3b: switching, before see-through.

## Performance
- The gun costs next to nothing: one mesh of a few thousand triangles.
- **The cost is the see-through view.** Each visible portal renders the scene again.
  - Mitigations: half-resolution render targets, update only while the portal is on screen, and a key to switch the view off.
  - Measure FPS before and after in step 4.

## Safety
- Single-player only. No game files are changed (an `LD_PRELOAD` launch option only).
- Saves are backed up before the first modded launch.
- Portal 2 assets are converted on this PC and never redistributed.
- The recon scratch (decompressed packages) stays outside this folder.
