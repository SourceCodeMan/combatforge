# Core module review — Pass 2

| Field | Value |
|---|---|
| **Date** | 2026-07-23 |
| **Pass** | 2 |
| **Status** | done |
| **Files** | 21 |

## Summary

Pass 2 is an independent CURRENT-code audit of `Source/CombatForge/Core/` (all 21 `.h`/`.cpp`). The phase/round state machine, multi-mode combat ends (Elimination / Skirmish / FFA / CTF / Dom / HP), vote sanitization, and listen-host OnRep broadcast pattern are generally solid. The highest-risk defects are all in `ACombatForgeGameMode`: Options “reset to spawn” can fully revive an eliminated round-elim corpse, headless pilot phantoms still count as permanently alive teammates, and armed demolition bombs are not torn down between Elimination rounds so a 15 s fuse can breach the frozen arena during Intermission/Freeze.

## Findings

### P2-C1. `RequestResetToSpawn` fully revives eliminated pawns (heal + fire)

- **Severity:** blocker
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1669`
- **Symptom:** Options → “Reset to spawn” (UI → `ACombatForgeCharacter::ServerRequestResetToSpawn` → GameMode) heals and reloads any authority pawn with no phase / alive / out-state gate.
- **Why:** `Health->ResetForRound(3)` clears `UPFHealthComponent::bEliminated` and restores collision/appearance. Weapon fire gates only on `IsFireAllowed()` + `bEliminated` (`PFWeaponComponent`), not `bAliveInRound`. Round-elim already left the corpse possessed with `bAliveInRound=false` / `OutKind=2` / elim move-lock — after reset the player is shootable and can shoot while still absent from win/alive bookkeeping. Same call always stamps round HP mode `3`, so even a legitimate live use during sudden death breaks 1-HP showdown.
- **Fix:** Reject unless `PS->bAliveInRound && PS->OutKind==0` and phase is Lobby/Build/Combat-Live (or whatever product allows). Pass current round HP (`bSuddenDeathRoundActive ? 1 : 3`). Never call `ResetForRound` on an out-for-round / waiting-respawn pawn; if recovery is desired for stuck-alive only, leave elim state untouched.
- **Confidence:** high

### P2-C2. Headless pilot phantom stays permanently `bAliveInRound` and skews Elimination

- **Severity:** major
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3597`
- **Symptom:** Fleet pilot boxes (`game` + `?listen` + `-nullrhi`) carry a local human PlayerState with no pawn (`RestartPlayer` correctly early-outs via `IsHeadlessServerPhantom` at `CombatForgeGameMode.cpp:626`). That PS is still team-assigned in `PostLogin`, marked alive every `StartNextRound` (`ServerSetAliveInRound(true)` at `:1539`), and counted by `RecountAlive` (`:3609` only checks `bAliveInRound && TeamId<=1`).
- **Why:** Phantom cannot be eliminated (no pawn / health). One team always has a free “alive” slot → `CheckElimVictory` / timer alive-compare never wipes that side. `CountHumans` / match-leader correctly exclude the phantom, but alive/ready/vote paths do not.
- **Fix:** Exclude `IsHeadlessServerPhantom()` from team auto-assign (or force TeamNone + no roster), from `ServerSetAliveInRound` loops, and from `RecountAlive`. Mirror the existing exclusions used in `CountHumans` / `RefreshMatchLeader` / `EmitMatchReport`.
- **Confidence:** high

### P2-C3. Ready + vote early-advance still require the headless phantom

- **Severity:** major
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3940`
- **Symptom:** `AreAllPlayersReady` skips bots/ghosts but not `IsHeadlessServerPhantom`. `CheckAllVotesIn` (`:3690`) similarly treats every non-bot PS as a required voter.
- **Why:** Phantom never presses Ready/Vote. Lobby all-ready auto-start never fires on pilot listen hosts (host force-start still works). Vote early-advance (T3) never fires; full `VotePhaseDuration` always runs (Finalize will auto-abstain the phantom). Same root class as P2-C2.
- **Fix:** Treat phantom like a bot in `AreAllPlayersReady`, `CheckAllVotesIn`, and `FinalizeVotePhase` abstain fill.
- **Confidence:** high

### P2-C4. Armed bombs survive Elimination Intermission / next Freeze

- **Severity:** major
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3020`
- **Symptom:** `DestroyBombs()` runs on Combat→Vote (`:1133`) and host force-lobby (`:1325`), but `EndRound` intermission path (`:3096`) and `StartNextRound` / `BeginLiveRound` never clear `ActiveBombs`. Bomb fuse is 15 s (`PFBombActor::FuseSeconds`); Intermission 7 s + Freeze 5 s = 12 s.
- **Why:** A plant in the last ~15 s of Live can detonate while `Phase==Combat` but `RoundState` is Intermission or Freeze. `APFBombActor::ServerDetonate` only guards `Phase == Combat` (not Live), so `ServerRemovePieceForMatch` still deletes structural pieces mid-intermission — permanent arena change between rounds. Proximity paint can also flip health elim flags outside Live (GameMode then ignores the elim notify).
- **Fix:** Call `DestroyBombs()` (and ideally defuse without detonate) at Live→Intermission (`EndRound`) and/or at `StartNextRound` before respawns. Optionally harden bomb detonate to require `RoundState==Live`.
- **Confidence:** high

### P2-C5. Mid-combat joiners always enter the live round alive (Elimination)

- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:413`
- **Symptom:** `PostLogin` during Combat Freeze/Live sets `bAliveInRound=true` and (on next pawn) full HP, with an explicit CONTRACT-GAP comment.
- **Why:** Late joiners skip the round’s risk, pad `AliveCounts`, and can flip elim-victory math. Smallest-consistent choice, but still a balance/fairness hole for Elimination.
- **Fix:** Product call: either spectate-until-next-round (`bAliveInRound=false`, death-cam style) or allow join only in Intermission/Lobby.
- **Confidence:** high

### P2-C6. `CheckAllVotesIn` ignores inactive/ghost PlayerStates

- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3690`
- **Symptom:** Early vote advance iterates raw `PlayerArray` with only `!IsABot() && !bHasVoted`. No `IsActiveRosterMember` filter (unlike Ready).
- **Why:** A briefly lingering human PS after a dirty disconnect can hold Vote open until the 20 s timer even after `Logout`’s best-effort destroy/scrub. Race is narrower than pre-scrub days but the filter asymmetry remains.
- **Fix:** Same active-roster (+ phantom) filter as Ready / human counts.
- **Confidence:** med

### P2-C7. `HostForceReturnToLobby` from Vote skips rating commit + can double-report

- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1310`
- **Symptom:** Mid-match quit tears down toys and `SetPhase(Lobby)` without `FinalizeVotePhase` / Results. Empty-server path in `Logout` (`:513`) calls `EmitMatchReport()` then `HostForceReturnToLobby` when last human leaves during Vote.
- **Why:** Arena JSON may already exist from `BeginMatchRecord` at Build→Combat, but `CommitMatchRecord` (votes/result append) only runs on Vote→Results. Host quit during Vote leaves incomplete community records. Empty-server abandon emits a report without going through Results (OK for progression) but never commits the rating file.
- **Fix:** On force return from Combat/Vote, call a shared “close match record” path (commit with current tally / draw result) before Lobby wipe; keep abandon `EmitMatchReport` single-shot.
- **Confidence:** med

### P2-C8. `FindFreeRosterIndex` reuses slot 11 when full

- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:3987`
- **Symptom:** When all 12 roster slots are taken, the function logs and returns `MaxRosterSlots - 1` instead of failing.
- **Why:** Two combatants share `OwnerIdx` / FFA TeamId collision risk → build refunds and elim attribution can hit the wrong PS.
- **Fix:** Refuse join / refuse `AddBot` when no free index; never alias.
- **Confidence:** high

### P2-C9. `RequestResetToSpawn` sudden-death HP (related to C1)

- **Severity:** minor
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameMode.cpp:1684`
- **Symptom:** Always `ResetForRound(3)` even when `bSuddenDeathRoundActive`.
- **Why:** Live (non-elim) use from Options during showdown restores full locational thresholds instead of 1-HP mode.
- **Fix:** Fold into C1 gate; pass `bSuddenDeathRoundActive ? 1 : 3`.
- **Confidence:** high

### P2-C10. `PFColors::ForTeam` maps every non-zero team to Team B

- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeTypes.cpp:47`
- **Symptom:** `return (Team == 0) ? TeamA : TeamB` — TeamId 255 / unknown → orange.
- **Why:** Harmless if all callers `% 2` first (pawn tint does); surprising for unassigned roster debug / bomb labels if raw TeamId leaks.
- **Fix:** Explicit `Team==1` → B, else neutral gray / TeamA.
- **Confidence:** high

### P2-C11. Identity path diverges from contract T24 text

- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgeGameInstance.cpp:91`
- **Symptom:** GUID persists under `UserSettingsDir()/CombatForge/Identity.json`, not `Saved/CombatForge/Identity.json` as written in contract T24.
- **Why:** Intentional portable-install fix (commented); contract text is stale. Behavior is fine; docs/contract drift only.
- **Fix:** Update contract / design note; keep UserSettingsDir.
- **Confidence:** high

### P2-C12. Client log ship has size validation only (no rate/volume cap)

- **Severity:** nit
- **Status:** open
- **File:** `/home/josh/projects/combatforge/Source/CombatForge/Core/CombatForgePlayerController.cpp:924`
- **Symptom:** `ServerShipClientLog_Validate` caps chunk length at 4000; client can flush 8 chunks/s indefinitely into host `Saved/ClientLogs/`.
- **Why:** LAN triage feature; a buggy/malicious client can grow host disk. Low risk for trusted playtest.
- **Fix:** Per-connection byte budget / drop after N MB / sample only Warning+.
- **Confidence:** med

## Subsystems audited clean

- **GameState replication + R9 host OnRep mirrors** — all listed replicated fields registered in `GetLifetimeReplicatedProps`; setters force host broadcasts where UI binds.
- **PlayerState budget spend/refund clamps** — double-refund cannot mint past max structural/prop budgets.
- **Vote ID sanitization (T30)** — range 1–8, dedupe, like∩dislike strip, combined cap 4 before tally.
- **Skirmish / FFA / objective end double-fire guards** — `EndSkirmish` / `EndFreeForAll` / `EndTeamScoreObjective` require `Phase==Combat` and clear timers.
- **Respawn timer C7 gate** — pending `RespawnVictimAtTeamSpawn` aborts if no longer Combat-Live.
- **Lobby→Build community inject + PlayOnly flash** — FFA forces PlayOnly; Improvement loads pieces before freeze.
- **Bomb plant lobby guard** — `ServerTryPlantBomb` requires explicit Combat phase (not only `IsFireAllowed`) to avoid PieceId recycle across ClearAll.
- **PFPaths ArenaDir / UserPrefs migration** — ProgramData / `-ArenaDir` / one-time legacy copy paths are coherent.
- **Lighting / warehouse stream** — dedicated servers skip; map swap unloads stream; no gameplay authority issues in Core.
- **Death cam → teammate spectate (T5)** — server-side retarget; Fire/ADS cycle gated dead-only; join catch-up does not false-breakout.

## File checklist

| File | Reviewed |
|---|---|
| `CombatForgeTypes.h` | yes |
| `CombatForgeTypes.cpp` | yes |
| `CombatForgeGameInstance.h` | yes |
| `CombatForgeGameInstance.cpp` | yes |
| `CombatForgeGameMode.h` | yes |
| `CombatForgeGameMode.cpp` | yes |
| `CombatForgeGameState.h` | yes |
| `CombatForgeGameState.cpp` | yes |
| `CombatForgePlayerState.h` | yes |
| `CombatForgePlayerState.cpp` | yes |
| `CombatForgePlayerController.h` | yes |
| `CombatForgePlayerController.cpp` | yes |
| `PFClientLogShip.h` | yes |
| `PFPaths.h` | yes |
| `PFPaths.cpp` | yes |
| `PFUserPrefs.h` | yes |
| `PFUserPrefs.cpp` | yes |
| `PFLightingSubsystem.h` | yes |
| `PFLightingSubsystem.cpp` | yes |
| `PFWarehouseStreamSubsystem.h` | yes |
| `PFWarehouseStreamSubsystem.cpp` | yes |
