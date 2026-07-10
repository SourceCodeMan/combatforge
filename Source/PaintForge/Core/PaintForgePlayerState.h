// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "Core/PaintForgeTypes.h"
#include "PaintForgePlayerState.generated.h"

/**
 * Per-player replicated truth (contract §3.2): team, roster slot, ready flag, build budgets,
 * round-alive state, and match scoring (T18). Server-mutated only, through the ServerSet*
 * mutators — each one broadcasts OnFlagsChangedEvent on the host too (R9).
 */
UCLASS()
class PAINTFORGE_API APaintForgePlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	APaintForgePlayerState();

	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  TeamId = 255;        // 0=A, 1=B, 255=unassigned
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  RosterIndex = 255;   // 0..11; matches FPFBuildPieceRec::OwnerIdx
	UPROPERTY(ReplicatedUsing=OnRep_Flags) bool   bReady = false;      // Lobby + BuildPhase (reset each phase)
	UPROPERTY(Replicated)                  bool   bHasVoted = false;
	UPROPERTY(Replicated)                  bool   bAliveInRound = false;
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  StructuralBudget = 30;  // B6; server-mutated only
	UPROPERTY(ReplicatedUsing=OnRep_Flags) uint8  PropBudget = 6;
	UPROPERTY(Replicated)                  uint16 Eliminations = 0;
	UPROPERTY(Replicated)                  uint16 TimesEliminated = 0;
	UPROPERTY(Replicated)                  int32  MatchScore = 0;         // T18 scoring
	UPROPERTY(Replicated)                  FString PlayerGuidHash;        // set via PC on join (T24)

	FPFOnPlayerStateFlagsChanged OnFlagsChangedEvent;  // broadcast from OnRep_Flags + server setters

	// Server-only mutators (GameMode / APFBuildGrid call these; they broadcast on host):
	void ServerSetTeam(uint8 NewTeam, uint8 NewRosterIndex);
	void ServerSetReady(bool bNewReady);
	void ServerSetBudgets(uint8 Structural, uint8 Props);
	bool ServerTrySpendBudget(EPFPieceType Type);      // false if empty; decrements correct pool
	void ServerRefundBudget(EPFPieceType Type);        // T19: called with the ORIGINAL builder's PS
	void ServerAddScore(int32 Delta);

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
