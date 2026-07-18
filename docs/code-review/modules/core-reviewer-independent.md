# Core module — independent reviewer pass

| Field | Value |
|-------|-------|
| **Date** | 2026-07-17 |
| **Reviewer** | reviewer persona (independent of prior inventory) |
| **Status** | done |

## Summary

The Core module is a mature, well-structured match authority: phase/round state machine, multi-mode combat (Elimination / Skirmish / FFA / CTF / Domination / Hardpoint), leaver/leader handling, listen-host OnRep patterns, and progression reporting are generally solid. Match-type siblings are cleanly separated and double-fire guarded. A small set of real correctness issues remain — most importantly Domination’s HUD/target mismatch, Elimination fall-death soft-reset, and the incomplete `RespawnMode` path — plus a few net/timer hygiene gaps that matter under multiplayer load.

## Issues

### Issue 1 -- Severity: bug
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3786-3804`
- Description: `ComputeEffectiveScaling()` stamps `RoundWinsToTake` for all continuous modes using `SkirmishTagTarget` (default 50), except CTF which uses `CaptureFlagTarget`. Domination’s actual win condition is `DominationTargetScore` (default 200) in `TickDominationScoring` (line ~2751). The combat HUD and scoreboard read `GS->RoundWinsToTake` and display “DOM · first to 50” while the match continues until 200 — players see a false win target.
- Suggestion: For Domination, set target from `DominationTargetScore` (clamp to the replicated field width). Keep Hardpoint/Skirmish/FFA on `SkirmishTagTarget` and CTF on `CaptureFlagTarget`.
- Status: fixed (2026-07-18)

### Issue 2 -- Severity: bug
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3247-3272`
- Description: Fall deaths (`ShooterTeam == 255`) always take the near-instant `RespawnVictimAtTeamSpawn` path and never clear `bAliveInRound` / never go through the Round-Elimination out-for-round path. In Elimination mode a player can soft-suicide (fall) to reposition mid-round without counting as out, while still incrementing `TimesEliminated`. That breaks last-team-standing integrity and enables intentional fall-reset exploits.
- Suggestion: In Round Elimination (`MatchType == Elimination` && `RespawnMode == RoundElimination`), treat fall death as a normal round elimination (out, death cam, `CheckElimVictory`). Keep near-instant respawn only for continuous modes (Skirmish/FFA/objectives) and Lobby warmup.
- Status: fixed (2026-07-18)

### Issue 3 -- Severity: bug
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:2866-2874` and `3353-3356`
- Description: `EPFRespawnMode::Respawn` short-circuits `CheckElimVictory` and respawns victims without clearing `bAliveInRound`. Elimination’s timer resolve (`ResolveRoundOnTimer`) still decides the winner by `AliveCounts`, which never drop when everyone respawns — so every timed round ends as a draw. The Respawn variant of Elimination is non-functional if ever enabled (defaults to RoundElimination today, so latent rather than default-path).
- Suggestion: Either wire Respawn-mode Elimination to team score/tag scoring (like Skirmish), or reject/hide `RespawnMode::Respawn` until implemented; do not leave a mode that can only draw.
- Status: fixed (2026-07-18) — timer resolves by TeamScores; elims credit shooter team

### Issue 4 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1291-1303`
- Description: `HostSetFormat` silently clamps any size &lt; 6 to 4 (and ≥6 to 6). T15 ≤2v2 scaling (`ComputeEffectiveScaling` format ≤2 → first-to-3 / max-5 / 60s) is unreachable for the default bots-on path. The smoke path also calls `HostSetFormat(2)` expecting 2v2, but gets 4v4 fill instead.
- Suggestion: Allow internal/debug sizes (1–6) when not from the lobby UI, or add an explicit 2v2 format if small-format scaling is still desired; keep the UI limited to 4/6 at the widget layer.
- Status: fixed (optional PR) — clamp 1–6; UI still 4/6 only

### Issue 5 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:860-876`
- Description: `SetPhase` clears phase/round/lobby timers but not `ObjectiveScoreTimerHandle` / `HardpointRotateTimerHandle`. Safe today because Dom/HP end paths and `HostForceReturnToLobby` clear them explicitly, and `DestroyObjectiveActors` also clears them — but any future `SetPhase` jump out of Live Dom/HP that forgets those clears will leave 1 Hz scoring/rotation running into Vote/Lobby.
- Suggestion: Clear both objective handles at the top of `SetPhase` (same belt as other phase machinery).
- Status: fixed (optional PR)

### Issue 6 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1603-1668` and `3174+`
- Description: Timed respawn lambdas (`RespawnVictimAtTeamSpawn`, lobby elim reset) are not cancelled on match end / phase change. After a tag-cap or abandon → Vote, a pending lambda can still heal/teleport a pawn during Vote/Results (authority still valid). Cosmetic/gameplay oddity more than a scoring bug (scoring already stopped).
- Suggestion: Track respawn handles per player or gate the lambda on `Phase == Combat && RoundState == Live` (and optionally `OutKind == 1`).
- Status: fixed (optional PR) — phase gate in lambdas

### Issue 7 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3363`, `3276-3279`, `3434`; `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgePlayerState.cpp`
- Description: Critical replicated fields `bAliveInRound`, `Eliminations`, `TimesEliminated`, and `bHasVoted` are mutated by direct assignment without `ForceNetUpdate()` (unlike the `ServerSet*` helpers). Clients rely on natural PlayerState net frequency; death-spectate gates (`OnFireWhileDead` / `ServerSpectateNext`) and scoreboard elim counts can lag a beat on pure clients.
- Suggestion: Route through small server setters that assign + `ForceNetUpdate()` (mirror `ServerSetOutForRound` / `ServerAddScore` patterns).
- Status: fixed (optional PR) — ServerSetAliveInRound / ServerAddElimination / …

### Issue 8 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:386-415`
- Description: Mid-combat joiners in Elimination enter `bAliveInRound = true` during Freeze/Live (documented CONTRACT-GAP). A late joiner can swing alive counts / round victory without a freeze, and does not receive sudden-death `RoundHP` if joining mid-SD. Acceptable as smallest implementation, but weak for competitive Elimination.
- Suggestion: Prefer join-as-spectator (`bAliveInRound = false`, death cam) until next Freeze, or spawn only on Intermission→next round.
- Status: partial — C2 SD HP fixed; full join-as-spec left as product choice

### Issue 9 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1130-1138`
- Description: Lobby/Build “Connected” count in `NotifyReadyChangedInternal` tallies every `PlayerArray` entry (except `IgnorePS`), not `IsActiveRosterMember` / humans-only. Ghosts or unexpected bot PS rows inflate the count; Ready logic itself excludes bots/ghosts via `AreAllPlayersReady`, so the main risk is rare false `Connected >= 2` with one human + a lingering row.
- Suggestion: Count with the same `IsActiveRosterMember` + human filter used elsewhere (`CountHumans`).
- Status: fixed (optional PR)

### Issue 10 -- Severity: suggestion
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1463` and health contract
- Description: Sudden death passes `RoundHP = 1` into `ResetForRound`. The hit model has evolved to multi-hit region thresholds; if `ResetForRound(1)` no longer means “one BB kills,” sudden-death rules (B1/B2: 1 HP) may not match design.
- Suggestion: Confirm `UPFHealthComponent::ResetForRound` semantics for `1` under the current region/threshold model; adjust Core or Health so SD is still effectively one lethal hit.
- Status: verified — `ResetForRound(1)` sets `bOneHitMode` (any BB eliminates)

### Issue 11 -- Severity: nit
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:136-143` vs `ServerSetPhaseEndTime`
- Description: Build early-end only restamps `PhaseEndServerTime` and does not refresh `PhaseDuration`. Any UI that normalizes remaining/duration (vote-style ring) will treat early end as a slice of the original 90s rather than a fresh 5s ring. Phase remaining seconds themselves are correct.
- Suggestion: When shortening a phase for early end, also set `PhaseDuration` to the new countdown length (or expose a separate early-end duration).
- Status: fixed (optional PR) — `ServerSetPhaseEndTime` restamps PhaseDuration

### Issue 12 -- Severity: nit
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/PFWarehouseStreamSubsystem.cpp:36-115`
- Description: Warehouse environment stream is evaluated once at `OnWorldBeginPlay`. If `pf.StreamWarehouseMap=1` and the host later switches ArenaMap Warehouse → Yard, the streamed Industrial_Warehouse level is not unloaded (shell swaps, stream does not). Default cvar is 0, so production impact is low.
- Suggestion: On arena map change (or GameState ArenaMap OnRep), unload stream when leaving Warehouse; only stream when map is Warehouse.
- Status: fixed (optional PR) — OnRep_ArenaMap → OnArenaMapChanged

### Issue 13 -- Severity: nit
- File: `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.h:40` vs contract T2
- Description: `BuildPhaseDuration` default is 90s (comment: Tom 2026-07-17); original contract T2 is 180s. Product choice, not a code defect — call out only so rules docs stay aligned.
- Suggestion: Keep design docs/host UI labels in sync with the 90s default.
- Status: verified — code is already 180 s (matches T2); LobbyStartCountdown=0 intentional

## Areas audited clean

- Phase machine ownership: single `SetPhase` writer; Build → Combat freeze grid + match record; Vote → Results commit + match report; Results → Lobby bot scrub + reset.
- Elimination loop: Freeze → Live → elim/timer → Intermission → next round; first-to-N / max rounds / sudden-death deadlock path; listen-host move locks.
- Skirmish / FFA / CTF / Dom / HP sibling paths: separate timers, abandon checks, double-fire guards on `End*` (`Phase != Combat` early-out).
- CTF: pickup / return / capture / drop-on-elim / leaver flag return; Domination CoD-style capture + income; Hardpoint sole-occupancy scoring + rotation reset.
- Bomb plant validation (Combat+Live only, structural piece, range, charge consume, refund on spawn fail) — Lobby plant PieceId recycle hole closed.
- Net authority patterns: GameState/PlayerState `ServerSet*` + manual OnRep for listen host; host RPCs gated by `IsHostController` (listen authority OR MatchLeader).
- Leaver handling: scrub ghosts, destroy human PS, recount + mode abandon, leader migration, empty-server → Lobby, leaver stat snapshot for match report.
- Join team balance on human counts (not bot-padded); bot trim / overflow reteam; FFA unique combat TeamIds.
- PlayerController: phase IMC table, join-in-progress catch-up without false breakout horn, elim move-lock Client RPC, death cam → teammate spectate, client log ship with validation.
- GameInstance identity GUID + SHA1 hash; net protocol override for build mismatch; frame cap on boot.
- Types/constants, vote category IDs, map defs (Warehouse/Yard), path migration for arenas, lighting subsystem local-only spawn (no dedicated server rig).

## File checklist

| File | Verdict |
|------|---------|
| `CombatForgeGameMode.h` | Clean API surface; multi-mode config complete |
| `CombatForgeGameMode.cpp` | Core issues above (Dom target, fall death, RespawnMode, timer hygiene); otherwise strong |
| `CombatForgeGameState.h/.cpp` | Clean replication + listen-host OnRep; community map path sanitization good |
| `CombatForgePlayerState.h/.cpp` | Clean mutators/OnRep; raw field writes from GameMode are the weak spot |
| `CombatForgePlayerController.h/.cpp` | Clean phase/input/host/spectate/log-ship; host guard solid |
| `CombatForgeGameInstance.h/.cpp` | Clean identity + net version + prefs apply |
| `CombatForgeTypes.h/.cpp` | Clean shared enums/structs/constants/map defs |
| `PFPaths.h/.cpp` | Clean stable arena dir + legacy migrate |
| `PFUserPrefs.h/.cpp` | Clean local prefs (not match-critical) |
| `PFLightingSubsystem.h/.cpp` | Clean local cosmetic rig; map intensity path frozen correctly |
| `PFWarehouseStreamSubsystem.h/.cpp` | OK default-off; map-switch unload nit |
| `PFClientLogShip.h` | Clean thread-safe ring buffer for client log ship |

## Verdict

**Production-usable Core with a few mode-specific correctness bugs.** Fix Domination `RoundWinsToTake` and Elimination fall-death handling before competitive playtests of those modes; treat `RespawnMode::Respawn` as unfinished. Remaining items are hardening (respawn cancel, ForceNetUpdate, join-as-spec, format clamp). No need for a rewrite — the architecture (single writer, end-timestamps, mode siblings, leaver/report pipeline) is sound.
