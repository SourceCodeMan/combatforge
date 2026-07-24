# Server-join giant/checker props — root cause + fix (2026-07-23 night)

Tom (after the alpha-15 deploy): "the box and the barrel still have a bug... extra large
shapes and black checkers with no skins. It works fine in LAN only. It's only when I join
the server. You have tried to fix this in the past and haven't been able to."

**Status: ROOT-CAUSED with log evidence, fixed, and verified against a local
server-join repro. NOT packaged / NOT pushed anywhere (Tom: "don't publish anything").**
Ships whenever the next package goes out — the fix is client-side code, so it needs a new
CLIENT build (protocol bump per the usual rule); the box server does not strictly need to
move for this one, but keeping both on one version stays the policy.

## Why every previous attempt missed

Previous sessions proved the assets ARE in the pak (UnrealPak listing), blamed version
skew (fixed by the NetProtocol gate — real, but a different bug), and fixed two genuine
join-race issues (idempotent visual swap, PostReplicatedAdd-before-BeginPlay). All of
those were real fixes for real bugs — and the symptom survived them because the actual
failure was an ENGINE-REFUSED CALL that logs only a PIE-category warning nobody greps for
in a packaged client log.

## The evidence (Tom's own itch client log, 5:16 PM join to the alpha-15 box)

`C:\Users\tomch\AppData\Roaming\itch\apps\combatforge\CombatForge\Saved\Logs\CombatForge.log`:

```
PIE: Warning: Calling SetStaticMesh on '...PFBuildGrid_2147478361.ISM_Barrel_Team0' but Mobility is Static.
PIE: Warning: Calling SetStaticMesh on '...ISM_Barrel_Team1' but Mobility is Static.
PIE: Warning: Calling SetStaticMesh on '...ISM_Boxes_Team0' but Mobility is Static.
PIE: Warning: Calling SetStaticMesh on '...ISM_Boxes_Team1' but Mobility is Static.
...
LogStaticMesh: Warning: Invalid material [MID_M_PF_GlassFade_...] used on Nanite static
  mesh [SM_Ind_War_Storage_Box_Cardboard_Set_01_A]. Only opaque or masked blend modes are
  currently supported... Default Material will be used in game.
```

Also in the same log, at boot: `BuildPieceVisuals: READY | props Barrel=SM_Ind_Aba_...
Boxes=SM_Ind_War_...` — the client LOADS the assets fine. It was never a cook/content gap.

Exactly 4 refusals = Barrel + Boxes × 2 teams. The Crate ISM is absent from the list
because it is the engine Cone BY DESIGN and never swaps — the warning set matches the code
path perfectly.

## Root cause 1 — "extra large shapes with no skins" (placed/dressing props)

`APFBuildGrid`'s ctor creates the 14 piece ISMs with `Mobility = Static` and engine
placeholder shapes; `EnsurePieceVisualsApplied()` later swaps props onto the real
warehouse meshes with `SetStaticMesh`. The engine REFUSES `SetStaticMesh` on a registered
Static component once the world has begun play (`AreDynamicDataChangesAllowed` gate) —
it logs that PIE warning and returns false.

- **Authority**: grid is dressed during world init, BEFORE begin-play → swap sticks →
  host/LAN always looked right.
- **Joining client**: the grid replicates in mid-match, long after begin-play → swap
  REFUSED → instances are stamped with warehouse-FITTED transforms
  (`PieceWorldTransform`) but render on the unit engine cylinder/cube → several-metre
  gray/unskinned shapes. Every REMOTE joiner sees it; the host never does. "Works on LAN"
  = Tom hosts on LAN; joining the box made him the remote for the first time.
- **Why it never self-healed**: the retry latch was `bPieceVisualsReady =
  IsFullyLoaded()` — a CONTENT test. Content loads fine on clients, so the latch closed
  while the swap had silently failed, and the retry poll never ran.

**Fix (`PFBuildGrid.cpp`):**
1. Prop-type ISMs (`PFIsProp`) are now created **Movable** — they were always
   runtime-mutable by design (mesh swap + transform re-stamp + add/remove); Static was a
   mis-declaration only enforced against late joiners. Structural ISMs never change mesh
   and stay Static.
2. The swap now CHECKS `SetStaticMesh`'s return: on refusal it flips mobility and retries
   once, and if still refused it logs `BuildGrid: prop mesh swap REFUSED on ...` and
   holds `bPieceVisualsReady=false` so the retry poll stays alive. A refused swap can
   never again be silent.

## Root cause 2 — "black checkers" (build ghost previews of box/barrel)

The build ghost applies its translucent `M_PF_GlassFade` MID to the REAL warehouse mesh
for prop previews — and those Megascans meshes are **Nanite**, which does not render
translucent blends. The engine substitutes the DEFAULT (checker) material, exactly as the
logged warning says.

**Fix (`PFBuildComponent.cpp`):** ghost components set `bDisallowNanite = true` before
registration, so previews render from the mesh's non-Nanite fallback and the translucent
tint works. Placed instances keep Nanite.

## Verification (local, nothing published)

Repro topology = the bug topology: headless `-server -nullrhi` on L_Graybox?listen +
a real net client (`UnrealEditor -game 127.0.0.1:7777`), fixed binaries, this machine.

Client log verdict:
- `Welcomed by server` (true NM_Client remote join)
- `BuildGrid: cohesion palette ... (visualsReady=1)` after the welcome
- **ZERO** `Calling SetStaticMesh` warnings, zero `swap REFUSED` — where Tom's alpha-15
  client log shows four refusals at the same point in the flow.

Not exercised: the ghost fix visually (needs the build tool interactively); its mechanism
is the engine's own documented remedy for the exact logged warning.

## For the next package

- Fix rides in the next client build (bump NetProtocol per standing rule).
- On Tom's first post-fix server join, grep his client log: the four `Calling
  SetStaticMesh` lines must be gone; if anything ever regresses, the new
  `BuildGrid: prop mesh swap REFUSED` warning is the tripwire.
