# Warehouse / environment art integration — plan & findings

_Status (2026‑07‑12): **full map stream is OFF by default** (`pf.StreamWarehouseMap 0`).
Streaming `Industrial_Warehouse` on first PIE builds 100+ Nanite meshes and freezes the
editor for many minutes at ~100% CPU. Arena uses shell + warehouse **materials** instead.
To enable the full map after a one-time editor open of the warehouse map (so meshes compile):
`pf.StreamWarehouseMap 1` then Play. VT must stay on (`r.VirtualTextures=True`)._

## What was downloaded (in `Content/`, currently untracked)
- **`Scene_Warehouse/`** — a ~6.9 GB Megascans **Industrial Warehouse** scene: hero map `Maps/Industrial_Warehouse.umap`, PackedLevel prop actors (`Ind_War_Rack/BoxStack/Frame/HandTruck…`), storage shelves / pallets / crates / beams / cables / walls, plus its own materials, sequences, and `VisualFramework/DemoRoom`.
- **`MaterialsScifi/`** — ~221 MB Sci‑Fi Panels pack (14 tiling panel material instances on `M_MasterBase` + `M_MasterMask`). Off‑theme for the airsoft/warehouse direction; hold as an optional build‑piece skin only if we ever do a sci‑fi map.

## ⚠️ Blocker #1 — Virtual Texturing is OFF
The project has no `r.VirtualTextures=True`. The Megascans scene's **meshes and materials expect VT on**; with it off they render untextured/broken. This is the first thing to fix, but it's a **project‑wide renderer change** (one‑time texture rebuild, affects the whole look) — flip it *with eyes on the result*, not blind:
```
# Config/DefaultEngine.ini  → [/Script/Engine.RendererSettings]
r.VirtualTextures=True
r.VirtualTexturedLightmaps=True   # optional
```
Restart the editor after; textures rebuild once. Nanite is already default‑on in 5.6.

## ⚠️ Blocker #2 — 7 GB in git
Committing `Scene_Warehouse/` to normal git bloats every clone to 7 GB. Pick one **before** committing content:
- **Git LFS** for `Content/**/*.uasset` (standard for UE + git). Best if the content must travel with the repo.
- **`.gitignore` the Megascans content** and treat it as locally‑installed (re‑add from Fab per machine). Lightest repo; fine because the assets live in your Fab library.
- Note: a **dedicated play‑server doesn't render**, so it likely doesn't need the heavy warehouse textures at all — only the client build does. That argues for gitignore + client‑only install.

## Architecture reminder (why this isn't drag‑and‑drop)
We run **one persistent level `L_Graybox`** with a **runtime‑spawned `APFArenaShell`** built from engine‑primitive cubes. The design (docs 02 §3.5, playbook) is **FUNCTION vs FORM**: keep the shell's collision/spawns/build‑grid exactly as they are, drape venue visuals over them. `PFArenaShell::BuildWarehouseDressing()` already exists for exactly this (cosmetic, `NoCollision`, Z ≥ 1400, off the play volume) — today it's gray cubes.

## Three integration options (recommend #1 → then #2)
1. **Backdrop drape (safest, reversible, in `PFArenaShell`).** After VT is on, swap the cosmetic dressing cubes (ceiling deck, trusses, dock doors, corner columns, wall ribs) for real warehouse structural meshes placed **outside/above the 64×40 m play rectangle**. Collision/build‑grid untouched. Big "we're in a warehouse" payoff for low risk. Verify perf (Nanite meshes) on the kids' machines.
2. **Surface reskin of the arena floor/walls.** Point `M_PF_ArenaFloor/Wall/Metal` at warehouse‑grade tiling surfaces. Caveat: our walls/floor are **scaled primitives**, so the material must be world‑aligned tiling; harvest plain textures onto our own non‑VT master (proven pattern) rather than adopting the Megascans VT materials wholesale.
3. **Play inside the real `Industrial_Warehouse` map.** Biggest visual win, biggest change — would replace the runtime‑shell model with a real authored level (spawns, build‑grid bounds, midline, warm‑up pen all need re‑homing in real geometry). Defer; it's a redesign, not a drape.

## The 3 shell constraints (from design notes) any warehouse layout must respect
1. Ceiling ≥ 1200 uu (12 m) over the play rectangle, or lower the per‑map build‑height cap.
2. The 64×40 m play rectangle stays **empty floor** — push shelves/pallets/crates to out‑of‑bounds dressing; cover is player‑built only.
3. Spawns (west/east columns) + neutral midline strip need a semantic home in the geometry (loading docks at the ends, central aisle = neutral strip).

## Recommended next session (fast, together)
1. Decide git‑LFS vs gitignore for the 7 GB. 2. Flip VT on, rebuild, confirm the demo map renders. 3. Do option #1 (backdrop drape) in `BuildWarehouseDressing()`, verify in a 2‑player PIE, tune scale/placement by eye. 4. Perf‑check on a kid's PC before the real test.
