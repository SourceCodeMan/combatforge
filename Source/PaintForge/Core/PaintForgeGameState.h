// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Core/PaintForgeTypes.h"
#include "PaintForgeGameState.generated.h"

class APaintForgePlayerState;

/**
 * Replicated match truth (contract §3.2). The GameMode (server) is the only writer, through
 * the ServerSet* setters below; clients react exclusively via OnReps. Every ServerSet* setter
 * manually invokes its OnRep after mutating so the listen host broadcasts too (§5 R9).
 * All timing is end-timestamps in server world seconds — never ticking counters (02 §2.1).
 */
UCLASS()
class PAINTFORGE_API APaintForgeGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	APaintForgeGameState();

	// ---- Replicated state (all registered in GetLifetimeReplicatedProps) ----
	UPROPERTY(ReplicatedUsing=OnRep_Phase)      EPFMatchPhase Phase = EPFMatchPhase::Lobby;
	UPROPERTY(Replicated)                       float  PhaseEndServerTime = 0.f;   // 0 = untimed (Lobby)
	UPROPERTY(ReplicatedUsing=OnRep_RoundState) EPFRoundState RoundState = EPFRoundState::None;
	UPROPERTY(Replicated)                       float  RoundStateEndServerTime = 0.f;
	UPROPERTY(Replicated)                       uint8  RoundNumber = 0;            // 1-based during Combat
	UPROPERTY(ReplicatedUsing=OnRep_Score)      uint8  TeamRoundWins[2] = {0, 0};
	UPROPERTY(ReplicatedUsing=OnRep_AliveCounts)uint8  AliveCounts[2]   = {0, 0};
	UPROPERTY(Replicated)                       bool   bSuddenDeath = false;
	UPROPERTY(ReplicatedUsing=OnRep_ElimFeed)   TArray<FPFElimEntry> ElimFeed;     // capped at 50, oldest trimmed
	UPROPERTY(ReplicatedUsing=OnRep_VoteTally)  FPFVoteTally VoteTally;
	UPROPERTY(Replicated)                       FString MatchId;                   // GUID string, set at Lobby→Build
	UPROPERTY(Replicated)                       uint8  TargetTeamSize = 4;         // match format: 4 (4v4) or 6 (6v6); bots fill to this
	UPROPERTY(Replicated)                       uint8  RoundWinsToTake = 4;        // resolved first-to-N (3 at ≤2v2); HUD pip count reads this

	// ---- Client-safe helpers ----
	float GetPhaseTimeRemaining() const;   // PhaseEndServerTime - GetServerWorldTimeSeconds(), clamped ≥ 0
	float GetRoundTimeRemaining() const;
	bool  IsFireAllowed()  const;          // Lobby || (Combat && Live)   (T21) — same predicate client & server
	bool  IsBuildAllowed() const;          // Build only
	APaintForgePlayerState* FindPlayerByRosterIndex(uint8 RosterIndex) const;

	// ---- Server-side setters (set property + manually invoke OnRep on listen host — R7) ----
	void ServerSetPhase(EPFMatchPhase NewPhase, float EndServerTime);       // GameMode only
	void ServerSetRoundState(EPFRoundState NewState, float EndServerTime);  // GameMode only

	// (intra) analogous setters for score/alive/feed/tally and the rest of the match record
	void ServerSetPhaseEndTime(float EndServerTime);         // re-stamp within a phase (lobby/build countdowns)
	void ServerSetRoundNumber(uint8 NewRoundNumber);
	void ServerSetTeamRoundWins(uint8 WinsA, uint8 WinsB);
	void ServerSetAliveCounts(uint8 AliveA, uint8 AliveB);
	void ServerAddElimEntry(const FPFElimEntry& Entry);
	void ServerSetVoteTally(const FPFVoteTally& NewTally);
	void ServerSetSuddenDeath(bool bNewSuddenDeath);
	void ServerSetMatchId(const FString& NewMatchId);
	void ServerSetTargetTeamSize(uint8 NewSize);             // 4 or 6; clamped [1,6]
	void ServerSetRoundWinsToTake(uint8 NewWins);            // resolved at Lobby→Build from the format
	void ServerResetMatchState();                            // Lobby→Build: wins/feed/tally/round wiped

	// ---- UI subscription points (broadcast from OnReps AND from server setters on host) ----
	FPFOnPhaseChanged        OnPhaseChangedEvent;
	FPFOnRoundStateChanged   OnRoundStateChangedEvent;
	FPFOnMatchScoreChanged   OnMatchScoreChangedEvent;
	FPFOnElimFeedChanged     OnElimFeedChangedEvent;
	FPFOnVoteTallyChanged    OnVoteTallyChangedEvent;
	FPFOnAliveCountsChanged  OnAliveCountsChangedEvent;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UFUNCTION() void OnRep_Phase();
	UFUNCTION() void OnRep_RoundState();
	UFUNCTION() void OnRep_Score();
	UFUNCTION() void OnRep_AliveCounts();
	UFUNCTION() void OnRep_ElimFeed();
	UFUNCTION() void OnRep_VoteTally();
};
