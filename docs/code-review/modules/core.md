# Core module review

| Field | Value |
|-------|-------|
| **Date** | 2026-07-17 |
| **Updated** | 2026-07-18 (independent reviewer re-pass) |
| **Files** | 21 |
| **Status** | done |
| **GitHub** | [#10](https://github.com/SourceCodeMan/combatforge/issues/10) |

## Summary

Core’s default play path (Lobby → Build → Combat for **Skirmish**/Elimination round-elim → Vote → Results) is coherent: single phase writer on `ACombatForgeGameMode`, listen-host OnRep manual broadcasts on `ACombatForgeGameState`, join-in-progress GS binding on the PC, and solid leaver/leader/bot fill handling.

Open residual risk is mode-specific correctness: **Respawn** Elimination (B1), Domination HUD target (C1), and **fall-death soft-respawn in Elimination** (C4). An independent reviewer pass (2026-07-18) re-confirmed B1/C1 and added C4; architecture verdict unchanged (production-usable, not a rewrite).

## Open findings

### B1. Respawn mode: every timed round is a draw → match ends 0–0
- **Severity:** major
- **Status:** fixed (2026-07-18) — timer resolves by per-round TeamScores; elims credit shooter team
- **File:** `Source/CombatForge/Core/CombatForgeGameMode.cpp:1967-1985` (also `3353-3356`, `2866-2874`)
- **Symptom:** With `EPFRespawnMode::Respawn` under Elimination, tags/respawns never decide a round. Equal roster sizes make every Live timer a draw (no round win). After max rounds the series stays 0–0 → sudden death → still equal alive → **match draw 0–0**. Unequal rosters make the **larger** team win every timer regardless of play. Respawn mode is effectively unusable as a competitive mode.
- **Why:** `NotifyPawnEliminated` Respawn branch only calls `RespawnVictimAtTeamSpawn` and returns **without** clearing `bAliveInRound`. `CheckElimVictory` no-ops when `RespawnMode == Respawn`. `ResolveRoundOnTimer` still uses “more alive wins / equal = draw,” so with everyone kept “alive” the timer cannot score the round by elim performance.
- **Fix:** Give Respawn Elimination its own resolve path (e.g. team elim counts / score during Live). Mirror Skirmish-style `TeamScores` or a per-round frag tally; do **not** use equal-alive draws as the sole timer rule. Or hide/reject the mode until implemented.
- **Confidence:** high

### C1. Domination HUD “first to N” uses Skirmish tag target (50), not Domination score (200)
- **Severity:** major
- **Status:** fixed (2026-07-18) — `ComputeEffectiveScaling` stamps `DominationTargetScore`
- **File:** `Source/CombatForge/Core/CombatForgeGameMode.cpp:3788-3804` (`ComputeEffectiveScaling`); contrast `61` (`DominationTargetScore`), `2751-2758` (`TickDominationScoring`)
- **Symptom:** In Domination, combat HUD / scoreboard / results mode line (all read `GameState->RoundWinsToTake`) show **“first to 50”** while the server ends the match at **`DominationTargetScore` (200)**. Loading-menu copy correctly says 200; in-match UI lies.
- **Why:** Continuous modes stamp `RoundWinsToTake` from `CaptureFlagTarget` **or** `SkirmishTagTarget`. Domination’s win logic uses `DominationTargetScore`, but scaling never publishes that value to the replicated HUD field.
- **Fix:** In `ComputeEffectiveScaling`, branch Domination to `DominationTargetScore` (clamp to 255). Keep Hardpoint/Skirmish/FFA on `SkirmishTagTarget` and CTF on `CaptureFlagTarget`.
- **Confidence:** high

### C4. Fall death always soft-respawns — free mid-round reset in Elimination
- **Severity:** major
- **Status:** fixed (2026-07-18) — Round Elimination treats fall as out-for-round
- **File:** `Source/CombatForge/Core/CombatForgeGameMode.cpp:3247-3272`
- **Symptom:** Falling off (ShooterTeam 255) near-instant respawns in **every** mode and never clears `bAliveInRound` / never goes out-for-round. In Elimination a player can soft-suicide to reposition mid-round without counting as out (still bumps `TimesEliminated`).
- **Why:** Explicit early branch: fall death → `RespawnVictimAtTeamSpawn` + return, before the Round-Elimination out path.
- **Fix:** In Round Elimination (`MatchType == Elimination` && `RespawnMode == RoundElimination`), treat fall death as a normal round elim (out, death cam, `CheckElimVictory`). Keep near-instant respawn only for continuous modes (Skirmish/FFA/objectives) and Lobby warmup.
- **Confidence:** high
- **Source:** independent reviewer pass 2026-07-18

### C2. Mid-combat join during sudden death gets full round HP (3), not 1
- **Severity:** minor
- **Status:** fixed (optional PR) — PostLogin + RestartPlayer stamp ResetForRound(1) when SD live
- **File:** `Source/CombatForge/Core/CombatForgeGameMode.cpp:411-415` (`PostLogin` alive flag only); spawn HP from default pawn / `RestartPlayer` without `ResetForRound(1)`
- **Symptom:** A player who joins while a sudden-death round is Live can enter with 3 HP while everyone else has 1 HP.
- **Why:** `PostLogin` only stamps `bAliveInRound` for Freeze/Live; it does not call `ResetForRound` with the active round HP. `StartNextRound` already uses `RoundHP = bSuddenDeathRoundActive ? 1 : 3` for the full roster.
- **Fix:** After mid-combat `RestartPlayer`, if sudden death active, `Health->ResetForRound(1)`. Prefer join-as-spectator until next Freeze for competitive Elimination.
- **Confidence:** med

### C3. Contract drift: Build duration / lobby countdown vs §3.2 defaults
- **Severity:** nit
- **Status:** verified (no code change) — `BuildPhaseDuration` is already **180 s** (matches T2). `LobbyStartCountdown = 0` is intentional (comment: freeze is the single spawn countdown). Contract still lists 5 s pre-match grace; product chose 0.

## Hardening (from independent pass — suggestions, not majors)

Not on the fix queue; track if polishing multiplayer hygiene:

| ID | Topic | File (approx) | Status |
|----|--------|----------------|--------|
| C5 | `HostSetFormat` clamps any size &lt;6 to 4 — smoke `HostSetFormat(2)` becomes 4v4 | GameMode ~1291 | **fixed** (allow 1–6; UI still 4/6) |
| C6 | `SetPhase` does not clear objective score/rotate timers | GameMode ~860 | **fixed** |
| C7 | Pending respawn lambdas not cancelled on match end | GameMode RespawnVictimAtTeamSpawn | **fixed** (gate on Combat+Live / Lobby) |
| C8 | Raw `bAliveInRound` / elim counts without `ForceNetUpdate` | GameMode + PlayerState | **fixed** (ServerSet* helpers) |
| C9 | Lobby “Connected” count not filtered like ready roster | GameMode ~1130 | **fixed** (`CountHumans`) |
| I11 | Early-end `PhaseDuration` not restamped | GameState ServerSetPhaseEndTime | **fixed** |
| I12 | Warehouse stream not unloaded on map switch | PFWarehouseStreamSubsystem | **fixed** (OnRep_ArenaMap) |
| I10 | SD `ResetForRound(1)` one-hit semantics | PFHealthComponent | **verified** (`bOneHitMode`) |
| I8 | Join-as-spectator mid Elimination | GameMode PostLogin | **skipped** (product; C2 is the minimal fix) |

Full independent writeup: [core-reviewer-independent.md](./core-reviewer-independent.md).

## Subsystems audited clean

- Phase machine ownership; Elimination / Skirmish / FFA / CTF / Dom / HP siblings (aside from C1/C4/B1)
- GameState listen-host OnRep pattern; fire/build gates; PC phase IMC + join catch-up (no false breakout horn)
- Death cam → teammate spectate; host RPC guards; leaver/leader/report pipeline
- GameInstance identity + net protocol; paths/prefs/log ship; lighting; warehouse stream (default off)

## File checklist

| File pair | Verdict | Notes |
|-----------|---------|-------|
| `CombatForgeGameMode.h/.cpp` | **findings** | B1, C1, C4, C2, C3 |
| `CombatForgeGameState.h/.cpp` | clean | Replication + ServerSet* OnRep |
| `CombatForgePlayerController.h/.cpp` | clean | GS bind, catch-up, spectate, host RPCs |
| `CombatForgePlayerState.h/.cpp` | clean | (C8 hygiene optional) |
| `CombatForgeGameInstance.h/.cpp` | clean | |
| `CombatForgeTypes.h/.cpp` | clean | |
| `PFPaths` / `PFUserPrefs` / `PFClientLogShip` | clean | |
| `PFLightingSubsystem` / `PFWarehouseStreamSubsystem` | clean | stream map-switch nit only |
