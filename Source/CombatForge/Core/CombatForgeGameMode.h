// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "GameFramework/GameModeBase.h"
#include "Core/CombatForgeTypes.h"
#include "CombatForgeGameMode.generated.h"

class ACombatForgeCharacter;
class ACombatForgeGameState;
class ACombatForgePlayerController;
class ACombatForgePlayerState;
class APFArenaShell;
class APFBuildGrid;
class APFBotController;
class APFControlPointActor;
class APFFlagActor;
class APFTargetDummy;
class UPFRatingSubsystem;

/**
 * Server-only phase + round state machine (contract §3.2). THE single writer of match phase
 * and round state: Lobby → Build → Combat (rounds: Freeze/Live/Intermission) → Vote → Results
 * → Lobby. First to 4 round wins, max 7 rounds, sudden-death tiebreak, side swap every round
 * (B1); scaled to first-to-3 / max-5 / 60 s at ≤2v2 (T15). Also spawns the runtime arena
 * (shell, grid, lights — 02 §3.5) and the warm-up pen dummies (T29).
 */
UCLASS()
class COMBATFORGE_API ACombatForgeGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ACombatForgeGameMode();  // sets DefaultPawnClass=ACombatForgeCharacter, PlayerControllerClass,
	                        // GameStateClass, PlayerStateClass

	// ---- Config (contract §3.2 / §4.3) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") EPFRespawnMode RespawnMode = EPFRespawnMode::RoundElimination;   // B1
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float BuildPhaseDuration   = 150.f;  // 2:30 build window (Creative + Remix; PlayOnly is forced to ~0). Was 180 (Tom 2026-07-23).
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float LobbyStartCountdown  = 0.f;     // 0: no pre-match grace — the per-round Freeze ("GET READY") is the single spawn countdown (was a double)
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float FreezeDuration       = 5.f;     // T6
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float RoundDuration        = 90.f;    // 60 at ≤2v2 (T15)
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float IntermissionDuration = 7.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float SuddenDeathDuration  = 60.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float VotePhaseDuration    = 20.f;    // T3
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float ResultsDuration      = 15.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") uint8 RoundWinsToTakeMatch = 4;       // 3 at ≤2v2
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") uint8 MaxRounds            = 7;       // 5 at ≤2v2
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float RespawnDelay         = 5.f;     // Respawn mode only (04 Variant B)
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") uint8 DefaultTeamSize      = 4;       // 4 (4v4) or 6 (6v6); bots fill to it
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") bool  bFillWithBots        = true;    // top each team up to the format size
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") EPFBuildMode DefaultBuildMode = EPFBuildMode::Creative;
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") EPFMatchType DefaultMatchType = EPFMatchType::Skirmish; // kids' default
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") EPFArenaMap  DefaultArenaMap  = EPFArenaMap::Warehouse; // boot map (host repicks in lobby)
	UPROPERTY(EditDefaultsOnly, Category="PF|Match", meta=(ClampMin="1", ClampMax="255")) uint8 SkirmishTagTarget = 50; // first team/player to N tags wins (≤255; shared by Skirmish + FFA + Dom/HP)
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float  SkirmishMatchDuration = 300.f;  // one continuous Live period (Skirmish + FFA + objectives)
	UPROPERTY(EditDefaultsOnly, Category="PF|Match", meta=(ClampMin="1", ClampMax="255")) uint8 CaptureFlagTarget = 3; // CTF: first team to N captures
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float  HardpointRotateInterval = 45.f; // Hardpoint: seconds per slot
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float  DominationCaptureSeconds = 15.f; // Dom: solo seconds to capture a NEUTRAL zone (enemy zone = 2x via neutralize; teammates speed it x min(N,3))
	UPROPERTY(EditDefaultsOnly, Category="PF|Match", meta=(ClampMin="10", ClampMax="1000")) int32 DominationTargetScore = 200; // CoD: 1 pt per owned zone per 5 s -> first to 200
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float  ObjectiveScoreInterval = 1.f;   // Dom/HP: score tick period

	// ---- The only phase mutator in the codebase ----
	void SetPhase(EPFMatchPhase NewPhase);            // server; updates GameState, stamps timers, side effects

	/**
	 * Dev / playtest: add seconds to the current phase or combat-round clock and reschedule the
	 * underlying FTimerHandle so the match actually waits (HUD alone is not enough). Console: pf.AddTime.
	 */
	void DevAddMatchTime(float Seconds);

	// ---- Cross-package server entry points ----
	// pkg-weapons calls when a player pawn's HP hits 0 (dummies do NOT route here):
	void NotifyPawnEliminated(ACombatForgeCharacter* Victim, const FPFPaintHitInfo& FinalHit);
	// pkg-core PC calls after PlayerState ready flag flips (Lobby early-start / Build early-end):
	void NotifyReadyChanged();
	// pkg-core PC forwards votes here; GameMode validates, tallies to GameState, forwards to UPFRatingSubsystem:
	void SubmitVote(ACombatForgePlayerController* Voter, EPFThumbVote Thumb,
	                const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds);
	// Host-only actions (from PC RPCs):
	void HostForceStart();                             // Lobby only
	void HostCycleTeam(ACombatForgePlayerState* Target); // Lobby only (T22)
	void HostReturnToLobby();                          // Results only
	/** Mid-match leave: host resets everyone to Lobby (playtest quit-to-menu). */
	void HostForceReturnToLobby();
	void HostSetFormat(uint8 NewTeamSize);             // Lobby only: 1–6 (UI offers 4/6; smoke uses 2)
	void HostSetFillWithBots(bool bFill);              // Lobby only: top teams with bots at Lobby→Build
	void HostSetBuildMode(EPFBuildMode NewMode);       // Lobby only: Creative / Improvement / Play-only
	void HostSetMatchType(EPFMatchType NewType);       // Lobby only: Elimination / FFA / Skirmish / …
	void HostSetArenaMap(EPFArenaMap NewMap);          // Lobby only: respawn the shell as the map's class
	void HostSetCommunityMap(const FString& FileName, const FString& Label); // Lobby: Improvement/PlayOnly
	/** Rebuild GS->CommunityMapCatalog from host ArenaDir (seeds + saved matches) for the active shell. */
	void RefreshCommunityMapCatalog();

	// Objective actors → GameMode (server). Carrier state is stamped on PlayerState.
	void NotifyFlagTouched(class APFFlagActor* Flag, ACombatForgePlayerState* Toucher);

	// Spawn transform for a player in the current round (side swap: even rounds swapped — B1):
	FTransform GetSpawnTransform(const ACombatForgePlayerState* PS) const;

	// Player-invoked "reset to spawn" (Options menu): full heal + refill, then teleport the pawn to its
	// current-phase team spawn. Server-authoritative; routed from the owning client via
	// ACombatForgeCharacter::ServerRequestResetToSpawn. Safe with a live pawn in any phase.
	void RequestResetToSpawn(ACombatForgeCharacter* Pawn);
	/** Per-player last reset-to-spawn time (P2-C1: rate-limits the Options rescue). Weak keys —
	 *  stale entries from leavers are harmless and tiny. */
	TMap<TWeakObjectPtr<ACombatForgePlayerState>, double> LastResetToSpawnAt;
	static constexpr double ResetToSpawnCooldownSec = 20.0;

protected:
	// ---- Engine overrides ----
	virtual void BeginPlay() override;
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual void RestartPlayer(AController* NewPlayer) override;

	// ---- (intra) round loop ----
	void StartNextRoundFromBuildEnd();                 // Build timer / early-end countdown target
	void StartNextRound();
	void BeginLiveRound();
	void ResolveRoundOnTimer();
	void CheckElimVictory();
	void EndRound(uint8 WinnerTeam);                   // 0/1 winner, 255 = draw round
	void OnIntermissionEnd();
	void StartSuddenDeath();

	// ---- (intra) Skirmish match type (team frag-count; siblings, never touch the Elimination path) ----
	void ResolveSkirmishOnTimer();                     // timer expiry: higher tags wins, tie = draw
	void EndSkirmish(uint8 WinnerTeam);                // 0/1 winner, 255 = draw → Combat→Vote jump
	void CheckSkirmishAbandon();                       // disconnect: end promptly if a whole team leaves

	// ---- (intra) FreeForAll (solo tags; siblings, never touch Elimination / Skirmish paths) ----
	void ResolveFreeForAllOnTimer();                   // timer: most TagCount wins, tie = draw
	void EndFreeForAll(uint8 WinnerRosterOrNone);      // roster index of winner, 255 = draw
	void CheckFreeForAllAbandon();                     // one combatant left → they win

	// ---- (intra) objective match types (CTF / Dom / HP — siblings; TeamScores win) ----
	void ResolveCaptureFlagOnTimer();
	void EndCaptureFlag(uint8 WinnerTeam);
	void CheckCaptureFlagAbandon();
	void ResolveDominationOnTimer();
	void EndDomination(uint8 WinnerTeam);
	void CheckDominationAbandon();
	void ResolveHardpointOnTimer();
	void EndHardpoint(uint8 WinnerTeam);
	void CheckHardpointAbandon();
	void TickDominationScoring();                      // 1 Hz: sole occupancy on each pad → TeamScores
	void TickHardpointScoring();                       // 1 Hz: sole occupancy on active hill → TeamScores
	void RotateHardpoint();                            // advance active control-point slot
	void SpawnObjectiveActors();                       // flags / control points from PFGrid layout
	void DestroyObjectiveActors();
	/** 4 ammo barrels at random field spots each combat start (refill mag+reserve). */
	void SpawnAmmoBarrels();
	void DestroyAmmoBarrels();
	/** Mid-field floating bomb charge (not spawn-default — pick up with F, plant with G). */
	void SpawnBombPickup();
	void DestroyBombPickup();
	void RespawnBombPickup();   // timer callback: put another charge at center after pickup delay

public:
	/** Server: plant a demolition bomb on a structural build piece (validated: combat-live, piece exists,
	 *  structural, one bomb per piece, planter is carrying a charge). Called from the pawn's ServerPlantBomb. */
	void ServerTryPlantBomb(class ACombatForgeCharacter* Planter, uint16 PieceId);
	/** Pickup claimed — hide it and schedule a new one after 5 s. */
	void NotifyBombPickupTaken();

protected:
	void DestroyBombs();
	void ClearAllFlagCarriers();
	APFFlagActor* GetFlagForTeam(uint8 Team) const;
	bool IsTeamScoreObjectiveMode(EPFMatchType Type) const;
	TOptional<uint8> WinnerIfTeamAbandoned(const ACombatForgeGameState& GS) const; // unset = both present; both empty = TeamNone
	uint8 WinnerByTeamScore(const ACombatForgeGameState& GS) const;               // tie = TeamNone
	void EndTeamScoreObjective(uint8 WinnerTeam, const TCHAR* ModeName); // shared Combat→Vote jump

	// ---- (intra) lobby / build countdowns ----
	void BeginLobbyStartCountdown(bool bForced);
	void CancelLobbyStartCountdown();
	void FinalizeVotePhase();
	// Leaver-aware cores: a leaver's PlayerState can linger in PlayerArray during Logout, so the
	// all-ready / all-voted scans must be able to exclude it explicitly.
	void NotifyReadyChangedInternal(const ACombatForgePlayerState* IgnorePS);
	void CheckAllVotesIn(const ACombatForgePlayerState* IgnorePS);

	// ---- (intra) world + players ----
	// Health contract (§3.4): the component never calls the GameMode; the GameMode subscribes
	// to each player pawn's OnEliminatedEvent (dummies subscribe to their own — T29).
	void HandlePawnHealthEliminated(class UPFHealthComponent* Health, const FPFPaintHitInfo& FinalHit);
	void SpawnArenaActors();
	void SpawnWarmupDummyFor(ACombatForgePlayerState* PS);
	void ResetPawnForRound(ACombatForgeCharacter* Pawn, ACombatForgePlayerState* PS, uint8 RoundHP);
	void TeleportPawnTo(ACombatForgeCharacter* Pawn, const FTransform& Transform);
	// Controller-agnostic per-round respawn (players AND bots): resets HP + teleports, or restarts if no pawn.
	void RespawnCombatant(ACombatForgePlayerState* PS, uint8 RoundHP);
	// Skirmish/Respawn: timed reset-in-place of an eliminated victim at its team spawn (no round-out).
	// DelaySec < 0 uses RespawnDelay default; fall deaths pass a short value for near-instant respawn.
	void RespawnVictimAtTeamSpawn(ACombatForgeCharacter* Victim, float DelaySec = -1.f);
	void RecountAlive();

	// ---- (intra) bots (fill teams to the selected format — server only) ----
	void FillBotsToFormat();                           // top each team up to TargetTeamSize with bots
	void TopUpLobbyLoadouts();                         // freeform-warmup: keep every combatant's ammo/grenades full
	void RemoveAllBots();                              // despawn every bot (controller + pawn + PlayerState)
	ACombatForgePlayerState* AddBot(uint8 Team);        // spawn a bot controller + PlayerState on Team
	void TrimOneBotFromTeam(uint8 Team);               // free a slot for a joining human
	bool TrimOneBotAnyTeam();                          // same, when the whole roster is full
	int32 GetTeamCountByKind(uint8 Team, bool bBotsOnly) const;
	void ApplyServerMoveLocks();
	void ResetPlayerMatchStats();

#if !UE_BUILD_SHIPPING
	/** -SmokeImprovement: seed-driven Lobby→Build inject check, then exit (playtest script). */
	void TickSmokeImprovement();
#endif

	// ---- (intra) queries ----
	ACombatForgeGameState* GetPFGameState() const;
	UPFRatingSubsystem* GetRatingSubsystem() const;
	void GetTeamCounts(int32& OutTeamA, int32& OutTeamB) const;
	int32 GetTeamSlotIndex(const ACombatForgePlayerState* PS) const;
	bool AreAllPlayersReady(const ACombatForgePlayerState* IgnorePS = nullptr) const;
	/** True if PS still has a live Controller (filters disconnect ghosts in PlayerArray). */
	static bool IsActiveRosterMember(const ACombatForgePlayerState* PS);
	/** Destroy orphaned human PlayerStates that no longer own a Controller. */
	void ScrubGhostPlayerStates(const ACombatForgePlayerState* KeepPS = nullptr);
	/** Keep GameState->MatchLeader pointing at a connected human (first-joiner wins; migrates on
	 *  leave). On a listen server this is always the host; on dedicated it is what makes the whole
	 *  match-config surface work at all (multiplayer-plan W1.1). */
	void RefreshMatchLeader(const ACombatForgePlayerState* ExcludePS = nullptr);
	/** Connected human count (bots + ghosts + an in-flight leaver excluded). */
	int32 CountHumans(const ACombatForgePlayerState* ExcludePS = nullptr) const;
	/** Periodic host log line for crash triage (who is connected, phase, scores). */
	void LogCrashBreadcrumb();
	/** 30 s sweep: a match with zero humans returns to Lobby. Logout already handles the graceful
	 *  case; this catches teardown orderings where the last human never reached Logout (client
	 *  crash / connection drop), which left pilot boxes wedged mid-match with only the phantom. */
	void SweepEmptyServer();
	/** First unclaimed roster slot, or 255 when every slot is taken — never aliases. (P2-C8) */
	uint8 FindFreeRosterIndex() const;
	/** FindFreeRosterIndex, but drops a bot first if the board is full. 255 = genuinely no room. */
	uint8 AcquireRosterIndexForJoin();
	void ComputeEffectiveScaling();
	FPFMatchResult MakeMatchResult(uint8 MatchWinner) const;
	/** Frozen match-report wire format (combatforge-api /v1/match-report; progression-plan §1).
	 *  Always archived to Saved/MatchReports/; on a fleet box it is also queued + POSTed
	 *  (idempotent on matchId server-side, so crash-resends are harmless). */
	void EmitMatchReport() const;

	/** Stat snapshot of a mid-match leaver, taken in Logout BEFORE the PlayerState is torn down —
	 *  otherwise a kid who force-quits (the common case) silently loses the whole match's
	 *  progression. Merged into EmitMatchReport with completed:false; live rows win on rejoin. */
	struct FPFLeaverRow
	{
		FString GuidHash;
		uint8   Team = 255;
		int32   Elims = 0, TimesElim = 0, Score = 0, Tags = 0, Builder = 0;
	};
	TArray<FPFLeaverRow> PendingLeaverRows;   // cleared at Lobby→Build (new match)
	void SnapshotLeaverForReport(const ACombatForgePlayerState* PS);
	static void SanitizeVoteIds(TArray<uint8>& InOutLiked, TArray<uint8>& InOutDisliked);

	// ---- (intra) runtime state ----
	UPROPERTY() TObjectPtr<APFArenaShell> ArenaShell;
	UPROPERTY() TObjectPtr<APFBuildGrid>  BuildGrid;
	UPROPERTY() TMap<TObjectPtr<ACombatForgePlayerState>, TObjectPtr<APFTargetDummy>> WarmupDummies;

	// Objective runtime (server-owned; actors replicate themselves).
	UPROPERTY() TObjectPtr<APFFlagActor> Flags[2];
	UPROPERTY() TArray<TObjectPtr<APFControlPointActor>> ControlPoints;
	int32 HardpointActiveSlot = 0;
	// Domination (CoD model): per-zone capture chains live on the zone actors; the mode only counts income ticks.
	int32 DominationIncomeTickCounter = 0;   // 1 Hz scoring tick -> income every 5th (1 pt/zone/5 s)

	FTimerHandle PhaseTimerHandle;           // Build / Vote / Results phase ends
	FTimerHandle RoundTimerHandle;           // Freeze / Live / Intermission steps
	FTimerHandle LobbyCountdownHandle;       // all-ready / force-start 5 s countdown
	FTimerHandle LobbyTopUpHandle;           // lobby warmup: looping ammo/grenade top-up
	FTimerHandle ObjectiveScoreTimerHandle;  // Dom/HP periodic scoring
	FTimerHandle HardpointRotateTimerHandle; // Hardpoint slot rotation
	FTimerHandle CrashBreadcrumbTimer;       // 30 s host roster dump for crash triage
	FTimerHandle EmptyServerSweepTimer;      // 30 s no-humans → back to Lobby (SweepEmptyServer)

	UPROPERTY() TArray<TObjectPtr<class APFAmmoBarrel>> AmmoBarrels;
	UPROPERTY() TArray<TObjectPtr<class APFBombActor>> ActiveBombs;   // live demolition bombs (combat only)
	UPROPERTY() TObjectPtr<class APFBombPickup> BombPickup;           // mid-field floating charge (one at a time)
	FTimerHandle BombPickupRespawnHandle;
	static constexpr float BombPickupRespawnSec = 5.f;

	// Effective match scaling (T15), computed at Lobby→Build from connected team sizes:
	uint8 EffectiveRoundWinsToTake = 4;
	uint8 EffectiveMaxRounds = 7;
	float EffectiveRoundDuration = 90.f;

	bool  bLobbyCountdownActive = false;
	bool  bLobbyCountdownForced = false;
	bool  bBuildEarlyEndActive = false;
	float BuildPhaseFullEndTime = 0.f;       // restore point if early-end ready count drops

	bool  bSuddenDeathRoundActive = false;   // current round is the sudden-death round
	bool  bSuddenDeathPlayed = false;
	bool  bPendingSuddenDeath = false;       // next round is sudden death
	bool  bPendingMatchOver = false;         // decided during intermission
	uint8 PendingMatchWinner = 255;
	FPFMatchResult PendingMatchResult;
	/** Improvement/PlayOnly base map id for BeginMatchRecord parent lineage (empty = from-scratch). */
	FString PendingParentArenaId;

	// ---- Forced 3-match cycle: Creative → Remix → Remix Swap (Tom 2026-07-24) ----
	/** Stage the NEXT Lobby→Build will run. Mirrored to GS->CycleStage at Build entry and again
	 *  when it advances at Results (so Lobby + the server browser advertise the upcoming match). */
	uint8 NextCycleStage = 0;
	/** The arena as PLAYED last match (snapshot at Build→Combat freeze, pre battle damage) —
	 *  the base the Remix / Remix Swap stages inject. */
	TArray<FPFBuildPieceRec> LastMatchPieces;
	/** Wheel runs for every mode with a build phase; PlayOnly and FreeForAll sit outside it. */
	bool IsCycleActive() const;
	/** Rewind the wheel to its start for the current BuildMode (host abort / empty server). */
	void ResetMatchCycle();

	float MatchStartServerTime = 0.f;        // stamped at Lobby→Build (matchDurationSec source)

#if !UE_BUILD_SHIPPING
	FTimerHandle SmokeImprovementTimer;
	int32 SmokeImprovementStep = 0;          // 0=wait player, 1=start countdown, 2=await Build, 3=done
	float SmokeImprovementElapsed = 0.f;
#endif
};
