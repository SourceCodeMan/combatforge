# Cohesion — material palette (arena + build)

## Intent
Make graybox arena cubes and player-built forts read as the **same CQB warehouse** as Megascans props, without forcing a character art decision.

## Approach (landed in code)
Shared runtime palette in `PFBuildPieceVisuals`:

| Role | Textures (warehouse → CC0 fallback) | Master |
|---|---|---|
| Floor concrete | `Ind_War_Floor_Concrete_Smooth_01` | `M_PF_ArenaFloor` (triplanar) |
| Wall concrete | facade (+ lift) / floor / CC0 | `M_PF_ArenaWall` |
| Metal rusty | sheet metal rusty / garage worn | floor master + metal maps |
| Metal roof | painted roof / rusty | floor master + roof maps |

- **Arena shell** (`ApplyCohesivePalette` in BeginPlay) remaps floor / walls / metal / ceiling to this stack.
- **Build grid** structural ISMs use the same MIDs per type (Wall/Floor/Ramp/Roof).
- **Props** keep native Megascans materials (real UVs).
- **Spawn / hazard marks** stay `M_PF_ArenaMark` (Color-driven).

## Guardrails
- Do **not** commit `Content/Materials/M_PF_BuildPiece.uasset` (local edit / ISM checker).
- Character Bandit vs Mannequin fidelity is **out of scope** here (art direction call).

## Why triplanar masters
Raw Megascans Surface MIs on non-uniform scaled cubes smear or go near-black. Arena masters use world-aligned texturing; rebinding warehouse maps keeps one palette without UV fights.

## Verify in play
Log lines:
```
BuildPieceVisuals: ... palette Wall=... Floor=... masters floor=1 wall=1
ArenaShell: cohesion palette applied (floor=1 wall=1 metal=1 roof=1)
BuildGrid: cohesion palette Wall=... Floor=...
```
Built walls/floors should match field floor tone; ramps rusty; roofs painted metal; draped props unchanged.
