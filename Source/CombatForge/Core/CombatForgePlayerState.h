// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "Core/CombatForgeTypes.h"
#include "CombatForgePlayerState.generated.h"

/**
 * Per-player replicated truth (contract §3.2): team, roster slot, ready flag, build budgets,
 * round-alive state, and match scoring (T18). Server-mutated only, through the ServerSet*
 * mutators — each one broadcasts OnFlagsChangedEvent on the host too (R9).
 */
UCLASS()
class COMBATFORGE_API ACombatForgePlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	ACombatForgePlayerState();

	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  TeamId = 255;        // 0=A, 1=B, 255=unassigned
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  RosterIndex = 255;   // 0..11; matches FPFBuildPieceRec::OwnerIdx
	UPROPERTY(ReplicatedUsing=OnRep_Flags) bool   bReady = false;      // Lobby + BuildPhase (reset each phase)
	UPROPERTY(Replicated)                  bool   bHasVoted = false;
	UPROPERTY(Replicated)                  bool   bAliveInRound = false;
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  StructuralBudget = 30;  // B6; server-mutated only
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  PropBudget = 6;
	UPROPERTY(Replicated)                  uint16 Eliminations = 0;
	UPROPERTY(Replicated)                  uint16 TimesEliminated = 0;
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint16 TagCount = 0;         // FreeForAll per-player tags (also useful HUD)
	UPROPERTY(Replicated)                  int32  MatchScore = 0;         // T18 scoring
	UPROPERTY(Replicated)                  FString PlayerGuidHash;        // set via PC on join (T24)

	// Objective carrier / capture state lives on PlayerState (NOT the Character) so elim/respawn
	// and UI can read it without Character changes. CTF uses the flag fields; Dom/HP HUD can read
	// StandingOnPoint when the GameMode stamps it.
	UPROPERTY(ReplicatedUsing=OnRep_Flags) bool  bCarryingFlag = false;
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8 CarriedFlagTeam = 255;  // which team's flag (0/1), 255 = none
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8 StandingOnPoint = 255;  // control-point index, 255 = none

	/**
	 * Local-player "you're out" UI (replicated so pure clients see the timer).
	 * 0 = playable / not out, 1 = waiting timed respawn (read RespawnAtServerTime),
	 * 2 = out for the rest of the round (Elimination mode — spectate).
	 */
	UPROPERTY(Replicated) uint8 OutKind = 0;
	/** Server world time when OutKind==1 player becomes playable again (GetServerWorldTimeSeconds). */
	UPROPERTY(Replicated) float RespawnAtServerTime = 0.f;

	FPFOnPlayerStateFlagsChanged OnFlagsChangedEvent;  // broadcast from OnRep_Flags + server setters

	// Server-only mutators (GameMode / APFBuildGrid call these; they broadcast on host):
	void ServerSetTeam(uint8 NewTeam, uint8 NewRosterIndex);
	void ServerSetReady(bool bNewReady);
	void ServerSetBudgets(uint8 Structural, uint8 Props);
	bool ServerTrySpendBudget(EPFPieceType Type);      // false if empty; decrements correct pool
	void ServerRefundBudget(EPFPieceType Type);        // T19: called with the ORIGINAL builder's PS
	void ServerAddScore(int32 Delta);
	void ServerAddTag();                               // FreeForAll: ++TagCount + flags refresh
	void ServerSetFlagCarry(bool bCarrying, uint8 FlagTeam); // CTF: set/clear carrier
	void ServerSetStandingOnPoint(uint8 PointIndex);         // Dom/HP: 255 = off point
	void ServerSetOutWaitingRespawn(float AtServerTime);     // timed respawn countdown for HUD
	void ServerSetOutForRound();                             // Elimination: out until next round
	void ServerClearOutState();                              // back in play / round reset
	void ServerSetAliveInRound(bool bAlive);                 // ForceNetUpdate so clients see elim gates promptly
	void ServerAddElimination();                            // shooter ++Eliminations
	void ServerAddTimesEliminated();                        // victim ++TimesEliminated
	void ServerSetHasVoted(bool bVoted);
	void ServerResetMatchCombatStats();                     // elims / tags / score / vote for new match

	/** True for the phantom LOCAL player a headless pilot box (game exe + ?listen + -nullrhi)
	 *  carries: it holds a controller but no human sits behind it. Excluded from match-leader
	 *  assignment, human counts, and heartbeat player counts (multiplayer-plan Phase 0). Always
	 *  false on rendering machines and on true dedicated servers (no local players there). */
	bool IsHeadlessServerPhantom() const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;

protected:
	UFUNCTION() void OnRep_Flags();

	// Team tint is visual state derived from replicated TeamId — MIDs never replicate, so every
	// machine (server, listen host, pure clients) applies it locally whenever the pawn pointer or
	// the team flag lands, in either order. Also recolors live pawns on a host team-cycle (T22).
	UFUNCTION() void HandlePawnSet(APlayerState* Player, APawn* NewPawn, APawn* OldPawn);
	void ApplyTeamColorToPawn(APawn* InPawn) const;
};
