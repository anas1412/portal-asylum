# MODLOG: Outlast portal gun

## Decisions (user, 2026-10-06)
- **Gun is the default held item.**
  - LMB = blue portal, RMB = orange portal.
  - Middle mouse = camcorder. Lowering the camcorder brings the gun back.
  - The gun auto-hides while Miles needs his hands: climbing, vaulting, doors, hiding, grabs, cutscenes.
- **The gun model is built in-engine at runtime** from the user's own Portal 2 files. A GL overlay is the fallback.
- **Enemies teleport through portals too.**

## Facts
- **Game:** native Linux build, Steam build 576074, `OLGame.x86_64`. Non-PIE, so symbol addresses are fixed.
  - It has a full `.symtab` (148k symbols). DWARF covers only Wwise, so there are no engine struct layouts.
- **Packages:** UE3, version 882/3, engine build 12048, cooker 136, LZO-compressed (`compflags=2`).
- **TCHAR is `wchar_t` (4 bytes)** on this port: FString and Exec strings are UTF-32.
- **Engine layout** (from disassembly):
  - `FName::Names` is a TArray of entries; flags are at +8 (bit 1 means unicode), the string at +0x18.
  - UObject: Outer 0x40, Name 0x48, Class 0x50.
  - UField::Next 0x60.
  - UStruct: Children 0x80, PropertiesSize 0x88. SuperStruct is assumed at 0x78; verify at runtime.
  - UProperty: ArrayDim 0x68, ElementSize 0x6c, Offset 0x90.
  - UBoolProperty BitMask 0xb0. UObjectProperty, UStructProperty and UByteProperty keep their class/struct/enum pointer at 0xb0.
- **vtable slots:** ProcessEvent 67, UGameEngine::Exec 77, UOLEngine::Tick 76.
  - FOutputDevice vtable: dtor, dtor, `Serialize(const wchar_t*, EName)`, Flush, TearDown.
- **Input** (`DefaultInput.ini`):
  - LMB = `OLA_Use` (use/interact), so **Use has to move** (to E) when LMB fires blue.
  - RMB = `OLA_ToggleCamcorder`, F = `OLA_ToggleNightVision`.
- **Saves:** none existed before the first modded launch (the game had never been run).

## Log
- 2026-10-06: Step 1, hook skeleton.
  - `src/olportal.cpp` hooks `UOLEngine::Tick` via its vtable slot and interposes `SDL_GL_SwapWindow` for screenshots.
  - Commands are appended to `run/cmd`: `exec`, `dump`, `shot`, `objs`. Output goes to `run/mod.log`.
  - `dev-run.sh` launches the game windowed at 1280x720.
