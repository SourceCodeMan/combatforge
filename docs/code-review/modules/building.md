# Building module review — Pass 2

| Field | Value |
|-------|-------|
| **Date** | 2026-07-23 |
| **Reviewer** | (agent, independent Pass 2) |
| **Scope** | `Source/CombatForge/Building/` only (current code) |
| **Status** | done |
| **Prior reports** | not read |

## Summary

The build stack is mature and playable: one replicated `APFBuildGrid` FastArray, shared `QueryPlacement` for ghost + authority, 14 ISMCs for classic pieces, runtime `APFBuildPieceActor`s for window/door/trap, map-parameterized shell (`APFArenaShell` / `APFYardShell`), and careful prop-visuals self-heal. Authority, budgets, refund identity, delete rate cap, and bomb demolish paths look sound.

Residual issues: **arenaId fingerprint still hashes Warehouse-only grid header** (multi-map identity drift), **tap-place can miss if Started+Released before Tick**, and a few rematch/cleanup nits. No default Lobby→RoundElimination blockers found in this folder alone.

## Findings

### P2-BD1. `ComputeArenaId` / half-hash grid header ignores per-map `CellsY` and `Levels`
- **Severity:** major
- **Status:** open
- **File:** `Source/CombatForge/Building/PFArenaSerialization.cpp:28-36`, `72-89`, `92-117`; contrast `BuildLayoutJson` at `137-147` and `CombatForgeTypes.h:278-281`
- **Symptom:** Warehouse and Yard seed layouts with the same piece list produce the **same** `arenaId`. Favorites / results / lineage keyed only on `arenaId` cannot distinguish maps. Comment in `CombatForgeTypes.h` claims CellsY is part of the arenaId grid-header so maps reject each other; the hash does not encode it.
- **Why:** `AppendGridHeader` always appends `PFGrid::CellsY` (10) and `PFGrid::Levels` (4). JSON correctly writes the active `GridCellsY` / derived levels, and catalog load gates on `cellsY`, but the content-addressed id does not.
- **Fix:** Thread active `CellsY` + `Levels` (or map id) into `AppendGridHeader` / `ComputeArenaId` / `ComputeHalfHash` (and re-seed or accept new ids for existing files). Keep the JSON `grid` block as the load gate.
- **Confidence:** high
- **Source:** this-pass

### P2-BD2. Sub-frame place click never sends `ServerPlacePiece`
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Building/PFBuildComponent.cpp:117-125`, `225-281`, `410-438`
- **Symptom:** A very short LMB tap (Started + Completed/Canceled in the same frame before `TickComponent`) places nothing; player must hold until a tick runs.
- **Why:** `OnPlaceStarted` only sets `bPlaceHeld = true`; turbo/place runs only in tick while held. Release clears the flag with no one-shot place.
- **Fix:** On place Started (or first frame of hold), attempt one place immediately when ghost slot is valid; keep turbo for hold. Optionally queue a pending place if Started fired with wheel closed.
- **Confidence:** high
- **Source:** this-pass

### P2-BD3. `ClearAll` does not reset `DeleteRateWindows`
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Building/PFBuildGrid.cpp:849-900` (`RateWindows.Empty()` only); map at `.h:206`
- **Symptom:** After Lobby→Build rematch within the same second, a player who already deleted heavily can still be `RateLimited` on deletes until the 1 s window rolls.
- **Why:** Placement windows are cleared; delete windows are not.
- **Fix:** `DeleteRateWindows.Empty()` next to `RateWindows.Empty()` in `ClearAll`.
- **Confidence:** high
- **Source:** this-pass

### P2-BD4. Prop support trace can snap Z off props / non-floor hits
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Building/PFGridMath.h:287-309` (`SupportTopSubZ`); ISMs block `PF_ECC_BuildTrace` in `PFBuildGrid.cpp:141`
- **Symptom:** Near tall props, ghost prop height can quantize to the wrong 300-multiple (e.g. barrel top ~220 → level 1 / Z=3), then fail `NoAnchor` or briefly show invalid. Intended supports are terrain/floor tops only (T25).
- **Why:** Downward BuildTrace hits any blocking piece (props included), then `round(Z/300)*3` without requiring a floor/terrain surface.
- **Fix:** Restrict support hits to field floor + floor-like pieces (channel filter, profile, or ignore prop comps), or project hit Z only when within epsilon of a level top.
- **Confidence:** med
- **Source:** this-pass

### P2-BD5. Structural pieces lost per-team tint (T8 drift, accepted in code)
- **Severity:** minor
- **Status:** open (design tradeoff)
- **File:** `Source/CombatForge/Building/PFBuildGrid.cpp:229-251` (“One MID per type (team tint dropped…)”)
- **Symptom:** Enemy and friendly walls/floors/ramps share one warehouse surface MID; team color is not readable on structure (props keep native mats; 14 ISMC slots still exist).
- **Why:** Cohesion palette prioritizes texture readability over `PFColors::ForTeam` on the two team ISMCs.
- **Fix:** Accent-only `Color`/emissive on MIDs per team, or collapse to 7 ISMCs + custom data later; document T8 supersession if intentional for ship.
- **Confidence:** high
- **Source:** this-pass

### P2-BD6. Client may `Destroy` replicated special piece actors on FastArray remove
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Building/PFBuildGrid.cpp:1170-1191` (`DestroySpecialPieceActor` client scan)
- **Symptom:** Usually OK; under load, possible “attempted to destroy non-authority actor” noise or brief double-teardown when server channel close and client PreReplicatedRemove race.
- **Why:** Server owns `APFBuildPieceActor` channels; clients also call `Destroy()` by PieceId when mirroring removal (SpecialPieces map is server-filled only).
- **Fix:** On non-authority, only drop local bookkeeping / wait for channel close; do not `Destroy()` replicated specials (or mark pending and hide).
- **Confidence:** med
- **Source:** this-pass

### P2-BD7. Place-and-eject teleports eliminated / non-combat pawns
- **Severity:** minor
- **Status:** open
- **File:** `Source/CombatForge/Building/PFBuildComponent.cpp:754-808`
- **Symptom:** Corpses or any `ACharacter` overlapping a new piece get teleported along the shortest escape during Build.
- **Why:** Iterator is all `ACharacter`; no `bEliminated` / owner filter (bots and dead still match).
- **Fix:** Skip eliminated health, optionally only living combatants.
- **Confidence:** high
- **Source:** this-pass

### P2-BD8. Midline fade arms with `Warning` log every Build entry
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Building/PFArenaShell.cpp:1234-1239`
- **Symptom:** Host/client logs a Warning-level MidWall diagnostic each time Build starts (noisy packaged playtests).
- **Why:** Left at Warning after a “silent fail” investigation.
- **Fix:** Downgrade to `Log`/`Verbose` once material path is trusted; keep Warning only on failure paths.
- **Confidence:** high
- **Source:** this-pass

### P2-BD9. `ComputeHalfHash` comment claims mirrored halves hash equal
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Building/PFArenaSerialization.cpp:110-111`
- **Symptom:** None for current callers (half hashes only written to JSON). Misleading for future balance/lineage use: team-B mirrored X/Rot still differ from team A.
- **Why:** Team byte is excluded; coordinates are not reflected into a side-local frame.
- **Fix:** Fix the comment to match T27, or transform team-1 coords into plot-local space before hashing if product wants mirror equality.
- **Confidence:** high
- **Source:** this-pass

### P2-BD10. Duplicate `#include "UObject/ConstructorHelpers.h"` in grid TU
- **Severity:** nit
- **Status:** open
- **File:** `Source/CombatForge/Building/PFBuildGrid.cpp:18`, `24`
- **Symptom:** None (compile-clean).
- **Why:** Include listed twice.
- **Fix:** Remove one.
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

- **FastArray mirror (B8):** `PostReplicatedAdd` / `PreReplicatedRemove` → `AddPieceLocal` / `RemovePieceLocal`; server mutates then mirrors; `bSupportRemoveAtSwap` + index remap on remove.
- **Shared placement predicate:** phase, plot (wall edge adjacency), height cap + slack, structural slots, prop AABB overlap both ways, anchor; budget/rate only in `TryPlacePiece`.
- **Authority:** Team/Owner from `PlayerState`; client Type/XYZ/Rot re-validated; rate 10/s place + delete; refund gated on `BuilderByPieceId` vs roster recycle (T19).
- **Special pieces:** server-spawned, replicated open/seal/trap; one-way seal deferred until close swing finishes; normal door nav-transparent for bot open loop.
- **Bomb demolish:** `ServerRemovePieceForMatch` no refund / works frozen; `BombedPieceIds` one-bomb; cleared on ClearAll.
- **Grid math:** single snap source; wall S/W canonicalize; ramp pitch; T25 floor tops; map Levels via `ActiveLevels` / `ActiveHeightCapUU`.
- **Ghost / turbo:** BuildTrace 1200, slot change or 0.15 s, client 8 RPC/s, deny flash, play-only ghost suppress, wheel freezes place.
- **Visuals self-heal:** no latch on incomplete soft-load; grid retry + wall-clock backoff on ghost path; prop mesh swap re-stamps transforms; Dorito = engine cone + roof triplanar.
- **Arena shell:** map def ctor geometry; open-field bounds + extended midline barrier; MidWall rides `SetMidlineBarrierActive` (host + client); spawn/build/warmup transforms; Yard facade + desert out of cohesion dress list.
- **Serialization parse:** grid header accept Warehouse/Yard only; piece rot/type/range guards; inject re-mints PieceId.
- **Seeds:** idempotent write; warehouse + yard `cellsY` variants; mirror helper for team B.

## File checklist

| File | Reviewed | Notes |
|------|----------|-------|
| `PFBuildGrid.h` / `.cpp` | yes | Core replication + validation; P2-BD3, BD5, BD6 |
| `PFBuildComponent.h` / `.cpp` | yes | Input, ghost, RPCs; P2-BD2, BD7 |
| `PFGridMath.h` | yes | Header-only math; P2-BD4 |
| `PFBuildPieceActor.h` / `.cpp` | yes | Door/window/trap; clean |
| `PFBuildPieceVisuals.h` / `.cpp` | yes | Soft-load, collision inject, palette |
| `PFArenaShell.h` / `.cpp` | yes | Geometry, MidWall, drape; P2-BD8 |
| `PFYardShell.h` / `.cpp` | yes | Open field + facade + desert |
| `PFArenaSerialization.h` / `.cpp` | yes | Fingerprints + JSON; P2-BD1, BD9 |
| `PFArenaSeed.h` / `.cpp` | yes | Six seed maps; clean for purpose |

**Counts:** 1 major · 5 minor · 3 nit · 0 blocker
