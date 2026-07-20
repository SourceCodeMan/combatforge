// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Core/CombatForgeTypes.h"
#include "CombatForgeGameState.generated.h"

class ACombatForgePlayerState;

DECLARE_MULTICAST_DELEGATE(FPFOnMatchLeaderChanged);

/**
 * Replicated match truth (contract §3.2). The GameMode (server) is the only writer, through
 * the ServerSet* setters below; clients react exclusively via OnReps. Every ServerSet* setter
 * manually invokes its OnRep after mutating so the listen host broadcasts too (§5 R9).
 * All timing is end-timestamps in server world seconds — never ticking counters (02 §2.1).
 */
UCLASS()
class COMBATFORGE_API ACombatForgeGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	ACombatForgeGameState();

	// ---- Replicated state (all registered in GetLifetimeReplicatedProps) ----
	UPROPERTY(ReplicatedUsing=OnRep_Phase)      EPFMatchPhase Phase = EPFMatchPhase::Lobby;
	UPROPERTY(Replicated)                       float  PhaseEndServerTime = 0.f;   // 0 = untimed (Lobby)
	/** Full length of the current phase when it was stamped (server seconds). Vote ring uses this. */
	UPROPERTY(Replicated)                       float  PhaseDuration = 0.f;
	UPROPERTY(ReplicatedUsing=OnRep_RoundState) EPFRoundState RoundState = EPFRoundState::None;
	UPROPERTY(Replicated)                       float  RoundStateEndServerTime = 0.f;
	UPROPERTY(Replicated)                       uint8  RoundNumber = 0;            // 1-based during Combat
	UPROPERTY(ReplicatedUsing=OnRep_Score)      uint8  TeamRoundWins[2] = {0, 0};
	UPROPERTY(ReplicatedUsing=OnRep_Score)      uint16 TeamScores[2]    = {0, 0};   // Skirmish tag counts (reuses OnRep_Score)
	UPROPERTY(ReplicatedUsing=OnRep_AliveCounts)uint8  AliveCounts[2]   = {0, 0};
	UPROPERTY(Replicated)                       bool   bSuddenDeath = false;
	UPROPERTY(ReplicatedUsing=OnRep_ElimFeed)   TArray<FPFElimEntry> ElimFeed;     // capped at 50, oldest trimmed
	UPROPERTY(ReplicatedUsing=OnRep_VoteTally)  FPFVoteTally VoteTally;
	UPROPERTY(Replicated)                       FString MatchId;                   // GUID string, set at Lobby→Build
	UPROPERTY(Replicated)                       uint8  TargetTeamSize = 4;         // match format: 4 (4v4) or 6 (6v6); bots fill to this
	/** When true, GameMode tops each team up to TargetTeamSize with bots at Lobby→Build. */
	UPROPERTY(Replicated)                       bool   bFillWithBots = true;
	UPROPERTY(Replicated)                       uint8  RoundWinsToTake = 4;        // resolved first-to-N (3 at ≤2v2); HUD pip count reads this
	UPROPERTY(Replicated)                       EPFBuildMode BuildMode = EPFBuildMode::Creative;  // build style
	UPROPERTY(Replicated)                       EPFMatchType MatchType = EPFMatchType::Skirmish; // objective (Skirmish = respawn default)
	/** Host-picked arena map. Geometry itself travels as the shell's ACTOR CLASS (GameMode respawns
	 *  it on change) — this enum exists for UI seeding/labels only. */
	UPROPERTY(Replicated)                       EPFArenaMap ArenaMap = EPFArenaMap::Warehouse;
	/** Host-picked community arena filename under Saved/Arenas/ (Improvement / Play-only). Empty = auto top-ranked. */
	UPROPERTY(Replicated)                       FString SelectedCommunityMapFile;
	/** Host-facing / lobby label for the selected community map. */
	UPROPERTY(Replicated)                       FString SelectedCommunityMapLabel;
	/**
	 * Server disk catalog for Remix/Play-Only (top 100 for the active shell). Replicated so dedicated
	 * match leaders and joined clients can pick maps that only exist on the box — client local disk
	 * is empty there. Updated at Lobby entry, after each match save, and on arena shell swap.
	 */
	UPROPERTY(ReplicatedUsing=OnRep_CommunityMapCatalog) TArray<FPFCommunityMapInfo> CommunityMapCatalog;
	/** Pieces injected at Lobby→Build (Improvement whole map, or Creative all-bot half). HUD reads this. */
	UPROPERTY(Replicated)                       uint16 CommunityBasePieces = 0;
	/** Match leader: the player who configures the match (type/mode/format/map) and can force-start.
	 *  On a LISTEN server this is always the host; on a DEDICATED server the GameMode assigns the
	 *  first human at PostLogin and migrates it at Logout. Every Host* RPC guard + every host-only
	 *  UI predicate resolves through this (multiplayer-plan W1.1) — never through raw authority. */
	UPROPERTY(ReplicatedUsing=OnRep_MatchLeader) TObjectPtr<ACombatForgePlayerState> MatchLeader;

	// ---- Client-safe helpers ----
	float GetPhaseTimeRemaining() const;   // PhaseEndServerTime - GetServerWorldTimeSeconds(), clamped ≥ 0
	float GetRoundTimeRemaining() const;
	bool  IsFireAllowed()  const;          // Lobby || (Combat && Live)   (T21) — same predicate client & server
	bool  IsBuildAllowed() const;          // Build only
	ACombatForgePlayerState* FindPlayerByRosterIndex(uint8 RosterIndex) const;
	/** Is this controller the match leader? Safe on every machine (reads the replicated field). */
	bool  IsMatchLeader(const APlayerController* PC) const;

	// ---- Server-side setters (set property + manually invoke OnRep on listen host — R7) ----
	void ServerSetPhase(EPFMatchPhase NewPhase, float EndServerTime);       // GameMode only
	void ServerSetRoundState(EPFRoundState NewState, float EndServerTime);  // GameMode only

	// (intra) analogous setters for score/alive/feed/tally and the rest of the match record
	void ServerSetPhaseEndTime(float EndServerTime);         // re-stamp within a phase (lobby/build countdowns)
	void ServerSetRoundNumber(uint8 NewRoundNumber);
	void ServerSetTeamRoundWins(uint8 WinsA, uint8 WinsB);
	void ServerSetTeamScores(uint16 ScoreA, uint16 ScoreB);   // Skirmish tags; reuses OnRep_Score broadcast
	void ServerSetAliveCounts(uint8 AliveA, uint8 AliveB);
	void ServerAddElimEntry(const FPFElimEntry& Entry);
	void ServerSetVoteTally(const FPFVoteTally& NewTally);
	void ServerSetSuddenDeath(bool bNewSuddenDeath);
	void ServerSetMatchId(const FString& NewMatchId);
	void ServerSetTargetTeamSize(uint8 NewSize);             // 4 or 6; clamped [1,6]
	void ServerSetFillWithBots(bool bFill);                  // Lobby only (GameMode gates)
	void ServerSetRoundWinsToTake(uint8 NewWins);            // resolved at Lobby→Build from the format
	void ServerSetBuildMode(EPFBuildMode NewMode);           // Lobby only (GameMode gates)
	void ServerSetMatchType(EPFMatchType NewType);           // Lobby only (GameMode gates)
	void ServerSetArenaMap(EPFArenaMap NewMap);              // Lobby only (GameMode gates + respawns shell)
	void ServerSetSelectedCommunityMap(const FString& FileName, const FString& Label);
	void ServerSetCommunityMapCatalog(const TArray<FPFCommunityMapInfo>& Maps); // host disk → all clients
	void ServerSetMatchLeader(ACombatForgePlayerState* NewLeader);   // GameMode only (PostLogin/Logout)
	void ServerSetCommunityBasePieces(uint16 Count);         // Lobby→Build inject result
	void ServerResetMatchState();                            // Lobby→Build: wins/feed/tally/round wiped

	// ---- UI subscription points (broadcast from OnReps AND from server setters on host) ----
	FPFOnPhaseChanged        OnPhaseChangedEvent;
	FPFOnRoundStateChanged   OnRoundStateChangedEvent;
	FPFOnMatchScoreChanged   OnMatchScoreChangedEvent;
	FPFOnElimFeedChanged     OnElimFeedChangedEvent;
	FPFOnVoteTallyChanged    OnVoteTallyChangedEvent;
	FPFOnAliveCountsChanged  OnAliveCountsChangedEvent;
	FPFOnMatchLeaderChanged  OnMatchLeaderChangedEvent;   // menus re-style their leader-only rows
	FPFOnMatchLeaderChanged  OnCommunityMapCatalogChangedEvent;   // Remix picker reloads from GS

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifeProps) const override;

protected:
	UFUNCTION() void OnRep_Phase();
	UFUNCTION() void OnRep_RoundState();
	UFUNCTION() void OnRep_Score();
	UFUNCTION() void OnRep_AliveCounts();
	UFUNCTION() void OnRep_ElimFeed();
	UFUNCTION() void OnRep_VoteTally();
	UFUNCTION() void OnRep_MatchLeader();
	UFUNCTION() void OnRep_CommunityMapCatalog();
};
