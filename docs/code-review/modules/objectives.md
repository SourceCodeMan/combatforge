# Objectives module review — Pass 2

| Date | 2026-07-23 |
| Status | done |

## Summary

`Source/CombatForge/Objectives/` is a small, well-scoped authority-first objective layer: Domination/Hardpoint volumes (`APFControlPointActor`), CTF flags (`APFFlagActor`), and pure grid math (`PFObjectiveLayout`). Capture math, occupancy filtering (alive + not eliminated), contested freeze, two-stage neutralize→capture, and flag pickup→GameMode handoff are coherent. Replication is minimal and intentional (visual/state on the wire; carrier pointer server-only). No blockers or majors in current source.

## Findings

### 1. `ServerInit` does not clear an outstanding drop-return timer
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Objectives/PFFlagActor.cpp:149`
- **Symptom:** If a flag actor were re-initialized while a drop timer was still armed, `HandleDropReturnTimer` could fire after the new home state is set (usually benign `ServerReturnHome`, but surprising).
- **Why:** `ServerGiveTo` / `ServerReturnHome` / `EndPlay` clear `DropReturnTimer`; `ServerInit` does not. Today flags are spawn-once + `ServerInit` and destroyed with the match, so this is latent.
- **Fix:** `GetWorldTimerManager().ClearTimer(DropReturnTimer);` at the top of `ServerInit` (authority path).
- **Confidence:** high
- **Source:** this-pass

### 2. Carried flag with live `CarrierPS` but no pawn freezes in place
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Objectives/PFFlagActor.cpp:243`
- **Symptom:** Flag can sit mid-air at the last carried location with `bCarried == true` if the carrier PlayerState still exists but `GetPawn()` is null and GameMode has not yet drop/returned the flag.
- **Why:** Tick only returns home when `CarrierPS` is invalid; missing pawn is a silent no-op for location. Elim/logout paths in GameMode normally drop/return — gap is a timing/order edge.
- **Fix:** If `bCarried && CarrierPS.IsValid() && CarrierPS->GetPawn() == nullptr`, call `ServerDropAt(GetActorLocation())` (or return home after a short grace).
- **Confidence:** med
- **Source:** this-pass

### 3. Header claims clients follow the carrier via tick
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Objectives/PFFlagActor.h:85`
- **Symptom:** Comment drift only.
- **Why:** `Tick` early-outs for non-authority when carried (`PFFlagActor.cpp:239`); clients depend on `SetReplicateMovement(true)`, not a follow-tick.
- **Fix:** Rephrase comment to “server follow-tick + movement replication.”
- **Confidence:** high
- **Source:** this-pass

### 4. Flag actor ticks every frame even when idle at home
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Objectives/PFFlagActor.cpp:34`
- **Symptom:** Unnecessary tick cost (two flags max — negligible).
- **Why:** `bStartWithTickEnabled = true` always; only carried path needs tick on authority.
- **Fix:** Enable tick on `ServerGiveTo`, disable on drop/return (mirror control-point pulse pattern).
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

| Subsystem | Notes |
|-----------|--------|
| **Capture chain (`ServerTickCapture`)** | Contested freeze, empty progress persist, owner wipe of enemy progress, opposing unwind with leftover roll-over, two-stage neutralize→capture, contributor cap `min(N,3)`. |
| **Occupancy (`ServerQueryOccupancy`)** | Authority-only; pawn overlap; filters `bAliveInRound` + `Health->bEliminated`; optional occupant list for HUD stamp consistency. |
| **CP visual / materials** | Master MID pattern (no MID-of-MID); soft `/Game` materials + engine fallback; pulse only while capturing and not contested; A/B/C letters. |
| **CP activation** | `ServerSetActive` clears capture state; hides + disables collision when inactive. |
| **Flag pickup** | Overlap bound server-only; elimination gate; delegates rules to `GameMode::NotifyFlagTouched`. |
| **Flag drop timeout** | 45s auto-return; cleared on pick-up / return / EndPlay. |
| **`PFObjectiveLayout`** | Header-only; flag homes + 3 CP slots from `PFGrid` + `CellsY`; no shell dependency. |
| **Replication surface** | CP: index/owner/active/capturing/progress/contested. Flag: team/home/atHome/carried. Carrier is correctly server-only. |

## File checklist

| File | Reviewed | Notes |
|------|----------|--------|
| `Source/CombatForge/Objectives/PFControlPointActor.h` | yes | API + replicated capture state clear |
| `Source/CombatForge/Objectives/PFControlPointActor.cpp` | yes | Capture math, occupancy, visuals, tick pulse |
| `Source/CombatForge/Objectives/PFFlagActor.h` | yes | Carrier non-replicated by design |
| `Source/CombatForge/Objectives/PFFlagActor.cpp` | yes | P2-O1–O4 |
| `Source/CombatForge/Objectives/PFObjectiveLayout.h` | yes | Pure layout helpers; clean |

**Counts:** 0 blocker · 0 major · 2 minor · 2 nit
