# Voting module review — Pass 2

| Date | 2026-07-23 |
| Status | done |

## Summary

`Source/CombatForge/Voting/` is a single GameInstance subsystem: match-record lifecycle (begin layout JSON → stage votes → commit result/votes), community catalog ranking, seed arenas, and half/whole arena load for Remix / bot fill. Server/host gating is consistent for the write path; list/load path is host-disk only with path-traversal guards and grid-row (Warehouse vs Yard) isolation. Screenshot-on-publish is correctly limited to commit. No blockers or majors.

## Findings

### 1. Empty `VoterGuidHash` skips defensive dedupe
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Voting/PFRatingSubsystem.cpp:159`
- **Symptom:** Two staged votes with empty voter id both append → inflated `votes` array if a caller ever stages without a hash.
- **Why:** Dedupe loop only runs when `!Vote.VoterGuidHash.IsEmpty()`. GameMode currently fills `PlayerGuidHash`, so production path is fine.
- **Fix:** Reject empty `VoterGuidHash` in `AddVote`, or treat empty as a single anonymous slot.
- **Confidence:** high
- **Source:** this-pass

### 2. `LoadCommunityArenaByFileName` / preferred-path load has no `IsServerContext` check
- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Voting/PFRatingSubsystem.cpp:411`
- **Symptom:** A pure client could load local `Saved/Arenas` basenames if something called this API; `LoadMostRecentArena` correctly refuses clients.
- **Why:** Path guard + grid filter exist, but net-mode gate is missing on the basename loader used by `PickCommunityArena`’s preferred branch.
- **Fix:** Early-return false when `!IsServerContext()` (or document “host-only” and assert). Call sites today are GameMode/server.
- **Confidence:** med
- **Source:** this-pass

### 3. Successful begin-record logs at `Warning`
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Voting/PFRatingSubsystem.cpp:136`
- **Symptom:** Healthy “SAVED map …” noise at Warning level (also commit at `:244`).
- **Why:** Useful for playtest visibility; wrong severity for long-term log hygiene.
- **Fix:** `Log` for success; keep `Warning` for discard/skip paths.
- **Confidence:** high
- **Source:** this-pass

### 4. `PickCommunityHalf` header still says “X translation”
- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Voting/PFRatingSubsystem.h:54`
- **Symptom:** Doc mismatch only.
- **Why:** Implementation mirror-reflects X (and flips rot) so forts face the right way (`PFRatingSubsystem.cpp:522`).
- **Fix:** Update the comment to “mirror-reflect half into TargetTeam’s plot.”
- **Confidence:** high
- **Source:** this-pass

## Subsystems audited clean

| Subsystem | Notes |
|-----------|--------|
| **BeginMatchRecord** | Server-only; discards stale open record; skips 0-piece arenas; layout via `FPFArenaSerialization`; parent id for Remix. |
| **AddVote** | Server-only; overwrite-on-duplicate hash; stages until commit. |
| **CommitMatchRecord** | Winner/draw, score, sudden death, duration; `voterWonMatch` false for all on draw; category ids → names with unknown drop. |
| **WriteRecordToDisk** | UTF-8 no BOM; screenshot only on commit (avoids match-start useless frame). |
| **ListTopCommunityMaps** | Client empty list; seed ensure; dedupe by arenaId best score; sort; cap 1–100; **CellsY** shell isolation. |
| **Load path** | Basename-only + `..` / slash reject; grid mismatch reject; content-hash `OutArenaId` for Remix parent. |
| **PickCommunityHalf** | Developed-side pick; wall/floor/prop-specific X reflect; rot flip for facing. |
| **PFShortMatchHex** | Stable 8-hex from GUID string; fallback `00000000`. |

## File checklist

| File | Reviewed | Notes |
|------|----------|--------|
| `Source/CombatForge/Voting/PFRatingSubsystem.h` | yes | Lifecycle API; P2-V4 doc nit |
| `Source/CombatForge/Voting/PFRatingSubsystem.cpp` | yes | Full record + catalog + half remap |

**Counts:** 0 blocker · 0 major · 2 minor · 2 nit
