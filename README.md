# Equipment Color Runtime 0.2.2

Native SKSE player-equipment recoloring for Skyrim SE 1.6.1170. Requires matching
SKSE64, Address Library and SKSE Menu Framework 3. No ESP or Papyrus scripts.
Keep the older **Equipment Recolorer** DDS mod disabled; it is a separate backup.

## Usage

Launch SKSE through MO2 with **Equipment Color Runtime** enabled. Load a test save,
equip gear, press **F1**, then **Equipment Color Runtime > Recolor equipment**.
Choose an item and Whole item or a texture section. Pick a color, strength and
brightness, then press **Apply**. Close F1 to see the result without menu blur.
Reset selected restores that item/section; Reset ALL colors clears the character's
settings. Save the game to retain choices in its SKSE co-save.

Settings are separate for each base item and diffuse-texture section. Two inventory
copies of the same base item share settings. NPCs, ground objects and inventory
preview models are not recolored. Mod-added gear is discoverable when it uses
supported player biped meshes and standard Skyrim lighting shaders.

## No generated DDS files

Original game/mod textures are still sampled. This mod does not generate, resize,
save or replace them. It adjusts diffuse RGB in a runtime pixel-shader variant,
preserves alpha and subsequent lighting, and restores the original shader and
constant-buffer binding immediately after each draw. One variant serves all colors
for a source shader. There is no color-dependent texture cache or clear-cache button.
Shader bytecode/variants occupy memory while their owning D3D shader lives.

There is still a limit of 256 saved item/texture sections, not a DDS size limit.
Resetting colors removes saved settings. No cache files are needed or deleted.

## Compatibility and verification

0.2.2 replaces the fragile Direct3D context-vtable hook that Skyrim's renderer was
rewriting. Draw functions are now intercepted at their code entry with per-method
trampolines; the context vtable is never modified. A shared reentrancy guard covers
all draw types and new renderer implementations are detected and hooked safely.

`build.ps1 -Test` builds the DLL and runs shader pixel-readback tests plus the actual
production capture/draw-hook tests. Hook tests check exact modified vtable slots,
six direct/indexed/instanced/indirect draw paths, negative base-vertex handling,
graphics-state restoration, unaffected draws and 1,000 repeated color changes.
Hook integration tests require hardware D3D11. Shader-only tests default to WARP;
set `ECR_TEST_HARDWARE=1` to run those on hardware too. WARP's mutable draw dispatch
is not supported for game recoloring. If a runtime/wrapper replaces installed draw
hooks, the game integration disables recoloring and reports the incompatibility.
The shader test also accepts a path to `Skyrim - Shaders.bsa` for bytecode validation.
These tests do not replace an in-game visual/compatibility test.

Only Skyrim 1.6.1170 is enabled. Skin, face, hair, eyes and unsupported material or
shader patterns are left unchanged. Custom PBR/effect shaders, external renderer
replacements and special weapon display systems are not guaranteed compatible.
Black source pixels stay black. Texture sections are not automatic material masks.

After updating, restart Skyrim; verify the version is 0.2.2 in the F1 page. Test two
different items, Apply, Reset, unequip/re-equip, first/third person and save/reload.
Weapons must be present in the player biped graph; draw them when testing.
Log: `Documents/My Games/Skyrim Special Edition/SKSE/EquipmentColorRuntime.log`.
An Applied status reports that a selected mesh reached the draw hook, not that a
human has confirmed the final screen color.

Source: `E:/MO2/dev/EquipmentColorRuntime`. Deploy DLL and PDB from
`build/windows/x64/releasedbg` into this mod's `SKSE/Plugins` with Skyrim closed.
The older DDS project/mod is not modified by this build.

## Third-party code

Uses alandtse CommonLibVR ng commit `82e137ffb500d5208ea4694f5db881800dd63de5`,
the SKSE Menu Framework 3 example API header, and Microsoft's DXBC checksum code
from DirectXShaderCompiler commit `b9e9f840b3ed530a7f67010ee899a02a613d3586`:
https://github.com/microsoft/DirectXShaderCompiler . License texts ship in `licenses`.
Legacy DirectXTex files in the development tree are not linked into this version.
