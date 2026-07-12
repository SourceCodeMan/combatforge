// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Core/PaintForgeTypes.h"
#include "PaintForgeGameMode.generated.h"

class APaintForgeCharacter;
class APaintForgeGameState;
class APaintForgePlayerController;
class APaintForgePlayerState;
class APFArenaShell;
class APFBuildGrid;
class APFBotController;
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
class PAINTFORGE_API APaintForgeGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	APaintForgeGameMode();  // sets DefaultPawnClass=APaintForgeCharacter, PlayerControllerClass,
	                        // GameStateClass, PlayerStateClass

	// ---- Config (contract §3.2 / §4.3) ----
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") EPFRespawnMode RespawnMode = EPFRespawnMode::RoundElimination;   // B1
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float BuildPhaseDuration   = 180.f;   // T2
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float LobbyStartCountdown  = 5.f;
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
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") EPFMatchType DefaultMatchType = EPFMatchType::Elimination;
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") uint16 SkirmishTagTarget    = 50;      // first team to N tags wins
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float  SkirmishMatchDuration = 300.f;  // one continuous Live period

	// ---- The only phase mutator in the codebase ----
	void SetPhase(EPFMatchPhase NewPhase);            // server; updates GameState, stamps timers, side effects

	// ---- Cross-package server entry points ----
	// pkg-weapons calls when a player pawn's HP hits 0 (dummies do NOT route here):
	void NotifyPawnEliminated(APaintForgeCharacter* Victim, const FPFPaintHitInfo& FinalHit);
	// pkg-core PC calls after PlayerState ready flag flips (Lobby early-start / Build early-end):
	void NotifyReadyChanged();
	// pkg-core PC forwards votes here; GameMode validates, tallies to GameState, forwards to UPFRatingSubsystem:
	void SubmitVote(APaintForgePlayerController* Voter, EPFThumbVote Thumb,
	                const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds);
	// Host-only actions (from PC RPCs):
	void HostForceStart();                             // Lobby only
	void HostCycleTeam(APaintForgePlayerState* Target); // Lobby only (T22)
	void HostReturnToLobby();                          // Results only
	void HostSetFormat(uint8 NewTeamSize);             // Lobby only: 4v4 / 6v6 (bots fill to it)
	void HostSetBuildMode(EPFBuildMode NewMode);       // Lobby only: Creative / Improvement / Play-only
	void HostSetMatchType(EPFMatchType NewType);       // Lobby only: Elimination / FFA / Skirmish / …

	// Spawn transform for a player in the current round (side swap: even rounds swapped — B1):
	FTransform GetSpawnTransform(const APaintForgePlayerState* PS) const;

protected:
	// ---- Engine overrides ----
	virtual void BeginPlay() override;
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

	// ---- (intra) lobby / build countdowns ----
	void BeginLobbyStartCountdown(bool bForced);
	void CancelLobbyStartCountdown();
	void FinalizeVotePhase();
	// Leaver-aware cores: a leaver's PlayerState can linger in PlayerArray during Logout, so the
	// all-ready / all-voted scans must be able to exclude it explicitly.
	void NotifyReadyChangedInternal(const APaintForgePlayerState* IgnorePS);
	void CheckAllVotesIn(const APaintForgePlayerState* IgnorePS);

	// ---- (intra) world + players ----
	// Health contract (§3.4): the component never calls the GameMode; the GameMode subscribes
	// to each player pawn's OnEliminatedEvent (dummies subscribe to their own — T29).
	void HandlePawnHealthEliminated(class UPFHealthComponent* Health, const FPFPaintHitInfo& FinalHit);
	void SpawnArenaActors();
	void SpawnWarmupDummyFor(APaintForgePlayerState* PS);
	void ResetPawnForRound(APaintForgeCharacter* Pawn, APaintForgePlayerState* PS, uint8 RoundHP);
	void TeleportPawnTo(APaintForgeCharacter* Pawn, const FTransform& Transform);
	// Controller-agnostic per-round respawn (players AND bots): resets HP + teleports, or restarts if no pawn.
	void RespawnCombatant(APaintForgePlayerState* PS, uint8 RoundHP);
	// Skirmish/Respawn: timed reset-in-place of an eliminated victim at its team spawn (no round-out).
	void RespawnVictimAtTeamSpawn(APaintForgeCharacter* Victim);
	void RecountAlive();

	// ---- (intra) bots (fill teams to the selected format — server only) ----
	void FillBotsToFormat();                           // top each team up to TargetTeamSize with bots
	void RemoveAllBots();                              // despawn every bot (controller + pawn + PlayerState)
	APaintForgePlayerState* AddBot(uint8 Team);        // spawn a bot controller + PlayerState on Team
	void TrimOneBotFromTeam(uint8 Team);               // free a slot for a joining human
	int32 GetTeamCountByKind(uint8 Team, bool bBotsOnly) const;
	void ApplyServerMoveLocks();
	void ResetPlayerMatchStats();

	// ---- (intra) queries ----
	APaintForgeGameState* GetPFGameState() const;
	UPFRatingSubsystem* GetRatingSubsystem() const;
	void GetTeamCounts(int32& OutTeamA, int32& OutTeamB) const;
	int32 GetTeamSlotIndex(const APaintForgePlayerState* PS) const;
	bool AreAllPlayersReady(const APaintForgePlayerState* IgnorePS = nullptr) const;
	uint8 FindFreeRosterIndex() const;
	void ComputeEffectiveScaling();
	FPFMatchResult MakeMatchResult(uint8 MatchWinner) const;
	static void SanitizeVoteIds(TArray<uint8>& InOutLiked, TArray<uint8>& InOutDisliked);

	// ---- (intra) runtime state ----
	UPROPERTY() TObjectPtr<APFArenaShell> ArenaShell;
	UPROPERTY() TObjectPtr<APFBuildGrid>  BuildGrid;
	UPROPERTY() TMap<TObjectPtr<APaintForgePlayerState>, TObjectPtr<APFTargetDummy>> WarmupDummies;

	FTimerHandle PhaseTimerHandle;           // Build / Vote / Results phase ends
	FTimerHandle RoundTimerHandle;           // Freeze / Live / Intermission steps
	FTimerHandle LobbyCountdownHandle;       // all-ready / force-start 5 s countdown

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

	float MatchStartServerTime = 0.f;        // stamped at Lobby→Build (matchDurationSec source)
};
