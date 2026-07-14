# M_PF_BuildPiece — material setup (art pass M1)

Two ways to create the build-piece master material. **Easiest: run the Python script.**

## Option A — run the generator (recommended)

1. In the editor: **Tools → Execute Python Script…** and pick `Scripts/create_build_material.py`
   (or in the Output Log cmd line: `py "D:/projects/combatforge/Scripts/create_build_material.py"`).
2. It creates `/Game/Materials/M_PF_BuildPiece`. Works with **no textures** (concrete-gray + team trim);
   to go photoreal, paste a Megascans surface's texture paths into the CONFIG block at the top and re-run.
3. It's idempotent — safe to re-run after tweaking CONFIG.

### Reviewer notes / known risks

# Review of `M_PF_BuildPiece_gen.py` (UE 5.6 Python API)

I checked every `unreal.*` class, method, and property against the UE 5.6 API. The material contract (`Color` as a vector param driving emissive, world-aligned base, texture-optional fallback) is sound and the core node classes are all real. But there are two categories of problems: **one hard crash bug at the very end**, and **a block of dead/garbage code** that clutters the graph and risks `NameError`. Details, then the corrected script.

## Findings

| # | Severity | Location | Problem |
|---|----------|----------|---------|
| 1 | **Blocker** | `MEL.layout_material_expressions(mat)` | This method does **not exist** on `MaterialEditingLibrary` in UE 5.6. It raises `AttributeError` *after* the whole graph is built but *before* `recompile_material`/`save_asset` — so every run "fails" and nothing is saved. Removed (guarded via `getattr`). |
| 2 | **Blocker (logic)** | First `try:` block ("placeholder guard") | Pure garbage: `_link_in.__self__` (a module function has no `__self__`), `obounds if False else obounds`, a `_link_out(...)` that actually executes and creates **orphan duplicate** `ObjectLocalBounds`/`TransformPosition` nodes, then swallows everything in `except: pass`. It builds junk nodes and only survives by luck. Deleted entirely. |
| 3 | High | Second `try:` block | Contains two no-op confusion lines (`... if False else None`, `_link_in.__wrapped__`) and creates `thickness` in the *first* (deleted) block — after removing block 1, `thickness` must be created here or you get `NameError`. Rewritten cleanly. |
| 4 | Medium | Reuse path | If the asset exists it logs a warning and then **appends a second copy** of every node onto the old graph. Fixed by calling `MEL.delete_all_material_expressions(mat)` on reuse for a clean, idempotent regen. |
| 5 | Low (verify) | `WorldAlignedTexture` pin names | The defensive candidate lists are the right approach. Real UE 5.6 names are input **`TextureObject`**, input **`WorldSize`**, output **`XYZ Texture`** — all present in the candidate lists, so this is fine, just noted. |

Everything else verified **correct** for UE 5.6: `AssetToolsHelpers.get_asset_tools().create_asset`, `MaterialFactoryNew`, `create_material_expression`, `connect_material_expressions`, `connect_material_property`, `recompile_material`, `delete_all_material_expressions`, `EditorAssetLibrary.does_asset_exist/save_asset`; expression classes `ScalarParameter`, `VectorParameter`, `TextureObjectParameter`, `MaterialFunctionCall`, `Constant`, `Constant3Vector`, `ComponentMask`, `Fresnel`, `ObjectLocalBounds`, `TransformPosition`, `WorldPosition`, `Subtract`, `Divide`, `Clamp`, `OneMinus`, `Max`, `Multiply`; enums `MaterialProperty.MP_*`, `MaterialSamplerType.SAMPLERTYPE_*`, and — importantly — **both** `transform_source_type` **and** `transform_type` correctly use `MaterialPositionTransformSource` (they are the same enum in C++). The `Color` vector-param contract is intact and connected to emissive, so it survives compilation and shows up on the MID.

## Corrected script



## Known-risk note

- **`WorldAlignedTexture` pin names** — I could not exhaustively confirm the exact function-input display strings for your precise 5.6 build. The script tries `TextureObject`/`WorldSize` (inputs) and `XYZ Texture` (output) with fallbacks. If a texture path is supplied and you see a `no matching input/output` `RuntimeError`, open the function in the editor, read the real pin labels, and add them to `WAT_TEXOBJ_IN` / `WAT_SIZE_IN` / `WAT_OUT`. **The default (no-texture) path never touches WorldAlignedTexture**, so a plain run compiles regardless.
- **`layout_material_expressions`** — this was the original script's crash point; it likely does not exist in 5.6. It is now called only via `getattr` and is purely cosmetic (auto-arranges nodes). If absent, nothing happens; recompile/save still run. No action needed.
- **`ObjectLocalBounds` output name** — verified as `Full Bounds Max`, but the candidate list `BOUNDS_MAX` covers variants. If the top-band `try` block logs "top-edge band unavailable", the material still compiles with a Fresnel-only rim (team color still reads); to restore the band, confirm the output label in the node's details and prepend it to `BOUNDS_MAX`.
- **Top-band correctness on rotated / non-axis-aligned pieces** — the band compares pixel world-Z to the transformed local-max-corner Z. This is exact for axis-aligned arena walls/floors (your case) but the "top" corner can be wrong for arbitrarily rotated instances. Not an API issue; only affects where the trim appears.
- **Idempotency** — reruns now call `delete_all_material_expressions` and rebuild in place, so you no longer need to delete the asset first. Any open Material Editor window for this asset should be closed before rerunning to avoid a stale-editor state.
- **Contract check after run** — open the material (or an instance) and confirm a **Vector** parameter named exactly `Color` appears in the parameter list and is wired to Emissive. That is the only hard C++ dependency; everything else is cosmetic.

## Option B — build it by hand (fallback)

The user wants a manual node-by-node recipe. This is a documentation task - return markdown directly. Let me write the recipe.

Let me produce the concise recipe now.
