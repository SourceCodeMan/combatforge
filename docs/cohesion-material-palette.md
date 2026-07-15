# Cohesion — material palette (arena + build)

## Intent
Make graybox arena cubes and player-built forts read as the **same CQB warehouse** as Megascans props.

## Approach
`PFBuildPieceVisuals::CreatePaletteMID` (used by arena shell + build grid):

| Priority | Source | Notes |
|---|---|---|
| **1 (preferred)** | Warehouse Surface **MIs** | Floor+walls: `Ind_War_Floor_Concrete_Smooth_01_A`. Metal: rusty / painted roof. Self-contained albedo. |
| **2 (fallback)** | `M_PF_ArenaWall` + texture rebind | Only if warehouse pack missing. |
| **NEVER** | `M_PF_ArenaFloor` | Miswired base color → **pure black**. |
| **NEVER** | `M_PF_BuildPiece` | ISM checker + local-edit guardrail. |

- Walls use the **floor** warehouse MI (facade MI was near-black on engine cubes).
- Props keep native Megascans mats.
- Marks stay `M_PF_ArenaMark`.

## Black floor/wall postmortem
Cohesion forced triplanar `M_PF_ArenaFloor` MIDs over working warehouse Surface MIs → pure black.
Metal roles also used the floor master. Fix: warehouse MIs first; never the black floor master.
This was a **material** bug, not lighting (props/characters still looked fine under the same lights).
