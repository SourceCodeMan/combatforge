# Breachworks (working title) — Pre-Playtest Code Review

> Generated 2026-07-10 by an adversarial multi-agent review (workflow wwolhvtfj) of the graybox **before its first run**. Each finding was independently verified by a skeptic agent that tried to refute it.
>
> **Headline: 0 blockers, 0 major, 10 minor.** Nothing here should block the first 2-player listen-server playtest — the default `RoundElimination` path and core loop verify clean. All findings are latent/edge-case polish to address **after** the playtest confirms the loop. No fixes were applied (none were safe to change blind in never-run code; several need design judgment or runtime confirmation — marked below).
>
> `file:line` refs are real but drift as code changes; verify before editing.

---

now# Breachworks — Pre-Playtest Review (2-Player Listen-Server PIE)

## Executive summary

**No blockers. No majors. The playtest can proceed.** All 11 confirmed findings are **minor** and every one of them is either latent (only fires in a non-default mode, a >2-player format, or under real WAN latency) or purely cosmetic. None break, block, or degrade the specific first run: 2 players joining from Lobby into a 1v1 RoundElimination match on a loopback listen server.

I also directly audited the mission's top blocker-risk subsystems and found them **clean**:

- **R1 — Enhanced Input lifetime:** solid. `InputConfig` is a `UPROPERTY` GC root (`PaintForgePlayerController.h:90`), all `IA_*`/`IMC_*` objects are UPROPERTY-reachable inside `UPFInputConfig`, the config is built exactly once, and `ApplyInputForPhase()` retries on a timer until the EI subsystem is alive rather than crashing or skipping (`PaintForgePlayerController.cpp:189-197`). No GC, no apply-too-early, no dead-input-after-a-minute.
- **IMC phase table:** phase changes only Add/Remove mapping contexts (never rebuild input objects), via an idempotent `SyncContext` lambda (`PaintForgePlayerController.cpp:240-254`). Clean.
- **Join-in-progress / initial replication order:** GameState binding retries until GS replicates in on the client (`TryBindGameState`, `PaintForgePlayerController.cpp:124-145`), then explicitly catches up on the already-replicated Phase and RoundState. No assumed-valid-too-early GS pointer.

The listen-server asymmetry hunt (R5) surfaced only bounded, sub-frame-on-LAN skews (corpse collision timing) and cosmetic timing races — nothing that diverges host vs. client in a way a tester would notice on loopback.

**Recommendation: run the playtest.** Treat everything below as post-playtest cleanup. The two worth fixing before you exercise non-default modes are the Respawn-mode draw bug and the vote-ring duration coupling.

---

## Findings (ranked by playtest impact)

All findings below are severity **minor** and verdict **CONFIRMED**. Ordered by how likely a tester is to notice, then by latent-severity if the feature is later exercised.

### 1. Respawn mode: every timed round is a draw → match ends 0-0
- **File:** `Source/PaintForge/Core/PaintForgeGameMode.cpp:725` (root cause at 968-988)
- **Symptom:** If the match ever runs in `EPFRespawnMode::Respawn`, no team can win a round on the timer; the match limps to max-rounds + a sudden-death round that also draws, ending 0-0.
- **Why:** In Respawn mode, `NotifyPawnEliminated` does a timed pawn reset and `return`s at line 987 — never setting `VictimPS->bAliveInRound=false` (line 994) nor calling `RecountAlive()` (line 1000). `bAliveInRound` is written only here, so `AliveCounts` stay pinned at full team size. `ResolveRoundOnTimer` (725-743) picks the winner purely from `AliveCounts[0]` vs `[1]`; equal counts → `Winner=TeamNone` (draw). `CheckElimVictory` is short-circuited in Respawn mode (752), so nothing else resolves it.
- **Fix:** Respawn mode needs a per-team frag/elimination count accrued during the round, fed into `ResolveRoundOnTimer`, since alive-count parity is meaningless when everyone respawns.
- **Confidence:** high. **Playtest impact:** none — `RespawnMode` defaults to `RoundElimination` (`PaintForgeGameMode.h:36`), the mode the 2-player test uses. Latent, but **effectively breaks the Respawn feature entirely** whenever that mode is enabled.

### 2. Vote countdown ring drains at the wrong rate if vote duration is retuned
- **File:** `Source/PaintForge/UI/PFVoteWidget.cpp:497` (constant at header `:97`)
- **Symptom:** The numeric vote timer stays correct, but the countdown RING visibly disagrees with it (starts partially drained, or freezes full then fast-drains) if a designer changes the vote duration.
- **Why:** `NativePaint` normalizes the ring with the hardcoded `UPFVoteWidget::VoteDuration = 20.f`, while the real phase length is `APaintForgeGameMode::VotePhaseDuration` (EditDefaultsOnly, default 20.f). The ring's numerator `GetPhaseTimeRemaining()` reflects the real duration; the denominator does not. They are duplicated, not shared.
- **Fix:** Drop the local constant; derive full-scale from replicated GameState — cache total phase duration on entry to Vote (`PhaseEndServerTime` minus entry time), or expose `VotePhaseDuration` on GameState and read it.
- **Confidence:** high. **Playtest impact:** none (both are 20.f today). Latent coupling that bites the first time the duration is retuned.

### 3. Missing `OnRep_RoundState()` in `ServerResetMatchState` (host broadcast gap)
- **File:** `Source/PaintForge/Core/PaintForgeGameState.cpp:221`
- **Symptom:** On the listen HOST, a RoundState change made inside `ServerResetMatchState` would not broadcast `OnRoundStateChangedEvent`; host-side UI/input reactions keyed to that event could miss it. Clients still see it via auto-OnRep.
- **Why:** The function sets `RoundState=None` directly but only manually invokes `OnRep_Score/AliveCounts/ElimFeed/VoteTally` — it omits `OnRep_RoundState`, breaking the file's own "set-then-invoke-OnRep-on-host" contract.
- **Fix:** Add `OnRep_RoundState();` alongside the other manual OnRep calls (after line 222). Ideally guard on an actual value change to avoid a redundant host-only re-broadcast.
- **Confidence:** med. **Playtest impact:** none — the only caller reaches this with RoundState already `None`, so the value never changes today. Latent asymmetry.

### 4. Breakout horn stub fires on join into an already-Live round
- **File:** `Source/PaintForge/Core/PaintForgePlayerController.cpp:165`
- **Symptom:** A client that binds GameState while the round is already `Live` triggers the breakout-horn path immediately on join, even though breakout already happened. (Horn is a Verbose-log NO-OP stub in v1, so the only real effect is one stray log line.)
- **Why:** `TryBindGameState`'s join-in-progress catch-up calls `HandleRoundStateChanged(GS->RoundState)` (line 144); that handler fires `PlayBreakout()` on ANY call where `NewState==Live` (171), with no guard distinguishing a genuine Freeze→Live transition from the catch-up seed.
- **Fix:** Track the previously-seen RoundState; fire the horn only on an actual transition into Live (prev==Freeze), not on the initial seed.
- **Confidence:** med. **Playtest impact:** none for the from-Lobby test (both players bind while RoundState==None). Only manifests on true join-in-progress into a Live round.

### 5. Round-pip row can briefly show the wrong pip count on a client
- **File:** `Source/PaintForge/UI/PFCombatHUDWidget.cpp:340`
- **Symptom:** On a client, the round-pip row can show the wrong number of pips (e.g. 4 vs 3) briefly after joining / as PlayerStates replicate in, then self-corrects on the next score change.
- **Why:** `HandleScoreChanged` infers first-to-3 vs first-to-4 format from `GS->PlayerArray.Num() <= SmallFormatMaxPlayers`. `PlayerArray` fills via independent PlayerState replication, so its count is transiently wrong on a client. The authoritative format (`EffectiveRoundWinsToTake`) is not replicated. An `FMath::Max3` guard prevents ever hiding a real win, so it's cosmetic.
- **Fix:** Replicate the resolved wins-to-take (or a small-format flag) on GameState at Lobby→Build and read that, instead of inferring from `PlayerArray.Num()`.
- **Confidence:** med. **Playtest impact:** none — in a 1v1 every transient PlayerArray value (0/1/2) is ≤4, so the format always resolves to 3. Only manifests in >2v2 formats.

### 6. Corpse collision disable time skews host vs. client
- **File:** `Source/PaintForge/Combat/PFHealthComponent.cpp:200` (host clock at `:115`)
- **Symptom:** Right after an elimination, a living player on the CLIENT can briefly collide/rubber-band against a corpse the HOST has already made walk-through. Window = one-way replication latency.
- **Why:** The 0.5s corpse collision-off timer runs off two clocks: host starts it in `ApplyPaintHit` at HP=0 (115); client starts it only when `bEliminated` replicates and `OnRep_Eliminated` fires (200). `SetCollisionResponseToChannel` is not replicated, so each machine flips its own corpse independently. (Note: host always disables first; the "or vice versa" in the original note is directionally impossible.)
- **Fix:** Acceptable as-is for v1 (code comments already accept this bounded skew). To tighten: replicate the server elim timestamp and compute the client delay as `CorpseBlockSeconds - (ClientNow - ServerElimTime)`.
- **Confidence:** low. **Playtest impact:** none on loopback (sub-frame window).

### 7. Slide timers not saved/restored across movement prediction replay
- **File:** `Source/PaintForge/Player/PFCharacterMovementComponent.h:93`
- **Symptom:** Under a server correction landing during/at the edge of a slide, the sliding CLIENT can briefly rubber-band or speed-pop (slide ends a few frames early/late, glide↔friction phase flips). Most visible under real latency.
- **Why:** `SlideElapsed`, `SlideRampStartElapsed`, `SlideCooldownRemaining`, `bSlideGlideActive` are plain CMC members not captured by `FSavedMove_PF`. On `ClientAdjustPosition` replay, these integrator timers keep live values instead of rewinding, so replayed slide sim diverges. Header comment (88-96) knowingly accepts this as v1 tolerance.
- **Fix:** Add the four fields to `FSavedMove_PF` — save in `SetMoveFor`, restore in `PrepMoveFor`, clear in `Clear`. Do NOT add them to `CanCombineWith` (continuously-varying floats would suppress move-combining and waste bandwidth); use a tolerance compare or omit.
- **Confidence:** med. **Playtest impact:** none — the listen HOST pawn is authoritative (never predicts/replays); the one client on near-zero-latency loopback gets infrequent, tiny corrections.

### 8. Slide can enter from a non-forward strafe (gating inconsistency)
- **File:** `Source/PaintForge/Player/PFCharacterMovementComponent.cpp:200`
- **Symptom:** A player at slide-entry speed but holding movement sideways/backward relative to facing (strafing at ≥750 after a turn) can trigger a slide; `EnterSlide` then boosts to 1150 along current velocity — an occasional unexpected sideways lunge.
- **Why:** Slide entry keys off the raw compressed flag `bWantsToSprintPF` (201) rather than `IsSprintingEffective()`, which enforces the forward-hemisphere `dot>0.5` rule used everywhere else (GetMaxSpeed/FOV/weapon). The flag has no directional gating.
- **Fix:** Gate slide entry on `IsSprintingEffective()`. Note: this alone won't fully kill the "sideways lunge" symptom, because `EnterSlide` boosts along velocity while `IsSprintingEffective` checks input direction — during a hard view-turn velocity can still lag off-axis. Consider also aligning the boost to facing.
- **Confidence:** med. **Playtest impact:** none — deterministic and identical host/client (not a replication issue); narrow edge case; documented as intended.

### 9. Sub-frame place-click can register no piece
- **File:** `Source/PaintForge/Building/PFBuildComponent.cpp:338`
- **Symptom:** A place click shorter than one frame (sub-~16ms tap) can occasionally place nothing.
- **Why:** `OnPlaceStarted` only sets `bPlaceHeld=true` (102); the actual `ServerPlacePiece` is polled in `TickComponent` (338) from `bPlaceHeld`. If Started and Completed both fire between two ticks, `bPlaceHeld` is already false when the tick runs. (A long PIE hitch on first run makes this marginally more likely.)
- **Fix:** Latch a `bPlaceQueued` flag on the press edge that the next tick consumes even if the button was released, so one fast tap always yields exactly one placement attempt.
- **Confidence:** med. **Playtest impact:** none at human click speed (a real click spans multiple frames).

### 10. Fast fire-tap out of a sprint is silently eaten
- **File:** `Source/PaintForge/Player/PaintForgeCharacter.cpp:302`
- **Symptom:** Press+release a fire tap inside the ~0.18s sprint-out window while sprinting → no shot fires; the tap is dropped.
- **Why:** `OnFirePressed` starts `SprintOutTimerHandle` instead of calling `StartFire` when sprinting (so `bWantsFire` never gets set); `OnFireReleased` then clears that timer before `OnSprintOutFinished` can run. The weapon's own sprint-out buffer is gated behind `bWantsFire`, which was never set, so it can't rescue the shot. Inconsistent with the HELD-fire raise-buffer that exists in the same code.
- **Fix:** If a quick tap should still fire, have `OnFireReleased` fire a single shot when `SprintOutTimerHandle` is still pending; otherwise document that a tap during sprint-out is intentionally consumed.
- **Confidence:** low. **Playtest impact:** none/negligible — at most one dropped shot when j-tapping out of a sprint; purely local input, no asymmetry.

---

## Subsystems audited and found clean
- **Enhanced Input lifetime / IMC application (R1):** GC-rooted config, once-built objects, retry-until-alive apply loop, idempotent per-phase IMC sync. No blocker.
- **Join-in-progress / initial replication order:** GameState bind retries until replicated, then catches up on Phase + RoundState. No too-early pointer assumptions on the controller path.
- **Round/phase state machine (default RoundElimination path):** sets `bAliveInRound=false` + `RecountAlive` + `CheckElimVictory` normally; resolves winners correctly for the 1v1 test.

## Clear-cut fixes to apply blind
None. The two **high-confidence** findings do not have a small/local/safe-to-apply-blind fix: the Respawn draw (GameMode:725) needs a new round-win metric (a design decision), and the vote-ring coupling (PFVoteWidget:497) needs GameState plumbing to read the real duration. The trivially-safe one-liners (e.g. adding `OnRep_RoundState()` at GameState:221) are only **med**-confidence on impact, so I've left them out of the blind-apply set. Recommend applying #1 and #2 manually with a quick review rather than blind.
