// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/CombatForgePlayerState.h"

#include "CombatForge.h"
#include "Player/CombatForgeCharacter.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"
#include "Net/UnrealNetwork.h"

bool ACombatForgePlayerState::IsHeadlessServerPhantom() const
{
	if (FApp::CanEverRender() || IsABot())
	{
		return false;   // rendering machine: any local player is a real human at a screen
	}
	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	return PC && PC->IsLocalController();
}

namespace
{
	// B6 budgets, tuned 4v4. GameMode resets to these at every Lobby→Build.
	constexpr uint8 MaxStructuralBudget = 30;
	constexpr uint8 MaxPropBudget = 6;
}

ACombatForgePlayerState::ACombatForgePlayerState()
{
	StructuralBudget = MaxStructuralBudget;
	PropBudget = MaxPropBudget;
}

void ACombatForgePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ACombatForgePlayerState, TeamId);
	DOREPLIFETIME(ACombatForgePlayerState, RosterIndex);
	DOREPLIFETIME(ACombatForgePlayerState, bReady);
	DOREPLIFETIME(ACombatForgePlayerState, bHasVoted);
	DOREPLIFETIME(ACombatForgePlayerState, bAliveInRound);
	DOREPLIFETIME(ACombatForgePlayerState, StructuralBudget);
	DOREPLIFETIME(ACombatForgePlayerState, PropBudget);
	DOREPLIFETIME(ACombatForgePlayerState, Eliminations);
	DOREPLIFETIME(ACombatForgePlayerState, TimesEliminated);
	DOREPLIFETIME(ACombatForgePlayerState, TagCount);
	DOREPLIFETIME(ACombatForgePlayerState, MatchScore);
	DOREPLIFETIME(ACombatForgePlayerState, PlayerGuidHash);
	DOREPLIFETIME(ACombatForgePlayerState, bCarryingFlag);
	DOREPLIFETIME(ACombatForgePlayerState, CarriedFlagTeam);
	DOREPLIFETIME(ACombatForgePlayerState, StandingOnPoint);
	DOREPLIFETIME(ACombatForgePlayerState, OutKind);
	DOREPLIFETIME(ACombatForgePlayerState, RespawnAtServerTime);
}

void ACombatForgePlayerState::ServerSetTeam(uint8 NewTeam, uint8 NewRosterIndex)
{
	if (!HasAuthority())
	{
		return;
	}
	TeamId = NewTeam;
	RosterIndex = NewRosterIndex;
	OnRep_Flags();
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerSetReady(bool bNewReady)
{
	if (!HasAuthority())
	{
		return;
	}
	if (bReady == bNewReady)
	{
		return;
	}
	bReady = bNewReady;
	OnRep_Flags();
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerSetBudgets(uint8 Structural, uint8 Props)
{
	if (!HasAuthority())
	{
		return;
	}
	StructuralBudget = Structural;
	PropBudget = Props;
	OnRep_Flags();
	ForceNetUpdate();
}

bool ACombatForgePlayerState::ServerTrySpendBudget(EPFPieceType Type)
{
	if (!HasAuthority())
	{
		return false;
	}

	if (PFIsProp(Type))
	{
		if (PropBudget == 0)
		{
			return false;
		}
		--PropBudget;
	}
	else
	{
		if (StructuralBudget == 0)
		{
			return false;
		}
		--StructuralBudget;
	}

	OnRep_Flags();
	ForceNetUpdate();
	return true;
}

void ACombatForgePlayerState::ServerRefundBudget(EPFPieceType Type)
{
	if (!HasAuthority())
	{
		return;
	}

	// 100% refund to the original builder (B6/T19), clamped at the per-match maximum so a
	// double-refund bug can never mint budget.
	if (PFIsProp(Type))
	{
		if (PropBudget >= MaxPropBudget)
		{
			UE_LOG(CombatForgeLog, Warning, TEXT("PlayerState %s: prop refund at full budget ignored"), *GetPlayerName());
			return;
		}
		++PropBudget;
	}
	else
	{
		if (StructuralBudget >= MaxStructuralBudget)
		{
			UE_LOG(CombatForgeLog, Warning, TEXT("PlayerState %s: structural refund at full budget ignored"), *GetPlayerName());
			return;
		}
		++StructuralBudget;
	}

	OnRep_Flags();
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerAddScore(int32 Delta)
{
	if (!HasAuthority())
	{
		return;
	}
	MatchScore += Delta;
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerAddTag()
{
	if (!HasAuthority())
	{
		return;
	}
	++TagCount;
	OnRep_Flags();   // HUD / scoreboard listeners
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerSetFlagCarry(bool bCarrying, uint8 FlagTeam)
{
	if (!HasAuthority())
	{
		return;
	}
	bCarryingFlag = bCarrying;
	CarriedFlagTeam = bCarrying ? FlagTeam : 255;
	OnRep_Flags();
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerSetStandingOnPoint(uint8 PointIndex)
{
	if (!HasAuthority())
	{
		return;
	}
	if (StandingOnPoint == PointIndex)
	{
		return;
	}
	StandingOnPoint = PointIndex;
	OnRep_Flags();
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerSetOutWaitingRespawn(float AtServerTime)
{
	if (!HasAuthority())
	{
		return;
	}
	OutKind = 1;
	RespawnAtServerTime = AtServerTime;
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerSetOutForRound()
{
	if (!HasAuthority())
	{
		return;
	}
	OutKind = 2;
	RespawnAtServerTime = 0.f;
	ForceNetUpdate();
}

void ACombatForgePlayerState::ServerClearOutState()
{
	if (!HasAuthority())
	{
		return;
	}
	if (OutKind == 0 && RespawnAtServerTime == 0.f)
	{
		return;
	}
	OutKind = 0;
	RespawnAtServerTime = 0.f;
	ForceNetUpdate();
}

void ACombatForgePlayerState::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// Fires on both server (possession) and clients (OnRep_PawnPrivate), covering the case
	// where the pawn pointer arrives after the team flag.
	OnPawnSet.AddUniqueDynamic(this, &ACombatForgePlayerState::HandlePawnSet);
}

void ACombatForgePlayerState::HandlePawnSet(APlayerState* /*Player*/, APawn* NewPawn, APawn* /*OldPawn*/)
{
	ApplyTeamColorToPawn(NewPawn);
}

void ACombatForgePlayerState::ApplyTeamColorToPawn(APawn* InPawn) const
{
	if (TeamId == 255)
	{
		return;   // unassigned — leave the default tint until a team lands
	}
	if (ACombatForgeCharacter* Character = Cast<ACombatForgeCharacter>(InPawn))
	{
		// FreeForAll uses unique combat TeamIds (roster slots) so FF-off never blocks tags;
		// palette still only has A/B — map via % 2 for a mixed look.
		Character->SetTeamColor(static_cast<uint8>(TeamId % 2));
	}
}

void ACombatForgePlayerState::OnRep_Flags()
{
	// Mirror the replicated team onto the pawn's MID tint (covers flag-after-pawn ordering and
	// host team-cycles recoloring an already-spawned pawn) before notifying UI listeners.
	ApplyTeamColorToPawn(GetPawn());
	OnFlagsChangedEvent.Broadcast();
}
