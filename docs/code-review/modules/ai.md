# AI module review — Pass 2

| Date | 2026-07-23 |
| Status | done |

## Summary

`Source/CombatForge/AI/` is the server-side bot brain (`APFBotController`) plus a tiny squad blackboard (`UPFSquadSubsystem`). The controller is large but deliberate: perception (sight cone + hearing), sticky target selection with point-blank / muzzle-LOS trade-ups, reaction delay only after true out-of-combat, tactical reposition scoring, navmesh `MoveTo` with projection/standoff, door open + jump over low barriers, objective goals for Dom/HP/CTF, smoke-aware LOS, and elimination-safe combat gates. Squad sharing is simple and correct for team modes. No blockers or majors in current source.

## Findings

### 1. CTF bots do not path to return their own dropped flag
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/AI/PFBotController.cpp:1158`
- **Symptom:** Own flag lying in the field is only returned if a bot happens to walk over it; AI prioritizes enemy-flag pickup or home-when-carrying, never “go return ours.”
- **Why:** `ComputeObjectiveGoal` for CTF: carry→home, else nearest non-carried **enemy** flag, else home. No branch for `OwnerTeam == MyTeam && !IsAtHome() && !IsCarried()`.
- **Fix:** If own flag is dropped, send a roster subset (or nearest bot) to its world location before idle-home defense.
- **Confidence:** high
- **Source:** this-pass

### 2. Single/Burst re-pull can double-fire on the first engage frame
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/AI/PFBotController.cpp:525`
- **Symptom:** Occasional extra first shot when a Single/Burst bot opens fire.
- **Why:** `SetFiring(true)` already `StartFire`s on the edge; same tick `TriggerPullTimer` (initial 0) immediately `StopFire`/`StartFire` again.
- **Fix:** Initialize `TriggerPullTimer` to the cadence on engage, or skip the re-pull on the same frame as the `SetFiring` edge.
- **Confidence:** med
- **Source:** this-pass

### 3. `EnsureObjectivesCached` treats either cache as “done”
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/AI/PFBotController.cpp:1123`
- **Symptom:** Only latent if both CP and flag actors ever needed after a partial first scan.
- **Why:** `if (bHavePoints || bHaveFlags) return;` — modes are exclusive today, so Dom fills CPs and CTF fills flags on first successful scan.
- **Fix:** Rescan when the active `MatchType` needs a cache that is empty/invalid.
- **Confidence:** high
- **Source:** this-pass

### 4. Squad lead never excludes self-report
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/AI/PFBotController.cpp:420`
- **Symptom:** None meaningful — same LKP as personal search.
- **Why:** `GetSharedLead(..., /*SelfExclude=*/nullptr, ...)` despite API support for excluding self.
- **Fix:** Pass the last-seen enemy actor when available (optional clarity).
- **Confidence:** high
- **Source:** this-pass

### 5. Squad board prunes only on `ReportEnemy`
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/AI/PFSquadSubsystem.cpp:46`
- **Symptom:** Stale weak entries linger until the next report (tiny N).
- **Why:** `GetSharedLead` filters by age/validity but does not remove; prune loop lives only in `ReportEnemy`.
- **Fix:** Optional prune pass in `GetSharedLead` or a low-rate tick — not required at current roster sizes.
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

| Subsystem | Notes |
|-----------|--------|
| **Combat aliveness gate** | Requires `bAliveInRound` and not `bEliminated` — corpse no longer path/shoot (Skirmish continuous modes). |
| **Target acquire / sticky** | FOV cone + proximity sixth sense; hysteresis; point-blank force switch; muzzle-LOS trade-up; eliminated filter. |
| **Aim / fire** | Pitch-preserving `UpdateControlRotation`; jitter + close-range tighten; injury/suppress error; fire along converged aim path; friendly body holds fire. |
| **Movement / nav** | Project goals to mesh; partial paths; standoff fallback (no GoalActor on live pawns); repath throttle; door open (normal doors only); jump stall. |
| **Tactical positions** | Hard body-clearance reject (anti roof-launch); cover/flank/spread/anchor scoring; vertical pursuit ring at enemy elevation. |
| **Objective goals** | Dom split by roster parity; HP active hill; CTF carry home / grab enemy flag. |
| **Perception / search** | Hearing → investigate; LKP hunt; consume reached search; squad de-clump offset. |
| **Ammo / fire mode** | Host refill when dry; deterministic mode from allowed mask + roster index. |
| **`UPFSquadSubsystem`** | Team-keyed sightings; dedupe by enemy; age filter on query. |
| **`pf.NavCheck` / `pf.BotSkill`** | Diagnostics and live skill override for new possesses. |

## File checklist

| File | Reviewed | Notes |
|------|----------|--------|
| `Source/CombatForge/AI/PFBotController.h` | yes | Tunables, skill enum, perception, tactics |
| `Source/CombatForge/AI/PFBotController.cpp` | yes | Full brain (~1.3k lines); P2-AI1–AI4 |
| `Source/CombatForge/AI/PFSquadSubsystem.h` | yes | Blackboard API |
| `Source/CombatForge/AI/PFSquadSubsystem.cpp` | yes | Report + lead; P2-AI5 |

**Counts:** 0 blocker · 0 major · 2 minor · 3 nit
