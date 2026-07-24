# M_PF_BuildPiece — material setup

Run the generator: **Tools → Execute Python Script…** → `Scripts/create_build_material.py`
(or Output Log cmd line: `py "D:/projects/combatforge/Scripts/create_build_material.py"`).

- Creates `/Game/Materials/M_PF_BuildPiece`. Works with **no textures** (concrete-gray + team
  trim); to go photoreal, paste a Megascans surface's texture paths into the CONFIG block at the
  top and re-run.
- Re-runs **delete and recreate** the asset (never wipe-in-place: `delete_all_material_expressions`
  on a loaded, rooted material can crash — `check(!IsRooted())`). Crash-safe and idempotent, but
  close any open Material Editor window on this asset first.
- `PF_DRY_RUN=1` in the environment reports what would be deleted/created and exits before
  touching anything.
- **C++ contract**: the material must expose a **Vector parameter named exactly `Color`** (the
  team tint the MIDs drive) and carry the ISM usage flag (the script sets it). After a run, open
  the material and confirm `Color` is present and wired — that is the only hard dependency.
- `WorldAlignedTexture` pin names vary slightly by engine build; the script tries candidate lists
  (`TextureObject`/`WorldSize` in, `XYZ Texture` out). The no-texture default path never touches
  that function, so a plain run always compiles.
