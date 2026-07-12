// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PaintForgePlayerState.h"

#include "PaintForge.h"
#include "Player/PaintForgeCharacter.h"
#include "Net/UnrealNetwork.h"

namespace
{
	// B6 budgets, tuned 4v4. GameMode resets to these at every Lobby→Build.
	constexpr uint8 MaxStructuralBudget = 30;
	constexpr uint8 MaxPropBudget = 6;
}

APaintForgePlayerState::APaintForgePlayerState()
{
	StructuralBudget = MaxStructuralBudget;
	PropBudget = MaxPropBudget;
}

void APaintForgePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(APaintForgePlayerState, TeamId);
	DOREPLIFETIME(APaintForgePlayerState, RosterIndex);
	DOREPLIFETIME(APaintForgePlayerState, bReady);
	DOREPLIFETIME(APaintForgePlayerState, bHasVoted);
	DOREPLIFETIME(APaintForgePlayerState, bAliveInRound);
	DOREPLIFETIME(APaintForgePlayerState, StructuralBudget);
	DOREPLIFETIME(APaintForgePlayerState, PropBudget);
	DOREPLIFETIME(APaintForgePlayerState, Eliminations);
	DOREPLIFETIME(APaintForgePlayerState, TimesEliminated);
	DOREPLIFETIME(APaintForgePlayerState, TagCount);
	DOREPLIFETIME(APaintForgePlayerState, MatchScore);
	DOREPLIFETIME(APaintForgePlayerState, PlayerGuidHash);
}

void APaintForgePlayerState::ServerSetTeam(uint8 NewTeam, uint8 NewRosterIndex)
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

void APaintForgePlayerState::ServerSetReady(bool bNewReady)
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

void APaintForgePlayerState::ServerSetBudgets(uint8 Structural, uint8 Props)
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

bool APaintForgePlayerState::ServerTrySpendBudget(EPFPieceType Type)
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

void APaintForgePlayerState::ServerRefundBudget(EPFPieceType Type)
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
			UE_LOG(PaintForgeLog, Warning, TEXT("PlayerState %s: prop refund at full budget ignored"), *GetPlayerName());
			return;
		}
		++PropBudget;
	}
	else
	{
		if (StructuralBudget >= MaxStructuralBudget)
		{
			UE_LOG(PaintForgeLog, Warning, TEXT("PlayerState %s: structural refund at full budget ignored"), *GetPlayerName());
			return;
		}
		++StructuralBudget;
	}

	OnRep_Flags();
	ForceNetUpdate();
}

void APaintForgePlayerState::ServerAddScore(int32 Delta)
{
	if (!HasAuthority())
	{
		return;
	}
	MatchScore += Delta;
	ForceNetUpdate();
}

void APaintForgePlayerState::ServerAddTag()
{
	if (!HasAuthority())
	{
		return;
	}
	++TagCount;
	OnRep_Flags();   // HUD / scoreboard listeners
	ForceNetUpdate();
}

void APaintForgePlayerState::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// Fires on both server (possession) and clients (OnRep_PawnPrivate), covering the case
	// where the pawn pointer arrives after the team flag.
	OnPawnSet.AddUniqueDynamic(this, &APaintForgePlayerState::HandlePawnSet);
}

void APaintForgePlayerState::HandlePawnSet(APlayerState* /*Player*/, APawn* NewPawn, APawn* /*OldPawn*/)
{
	ApplyTeamColorToPawn(NewPawn);
}

void APaintForgePlayerState::ApplyTeamColorToPawn(APawn* InPawn) const
{
	if (TeamId == 255)
	{
		return;   // unassigned — leave the default tint until a team lands
	}
	if (APaintForgeCharacter* Character = Cast<APaintForgeCharacter>(InPawn))
	{
		// FreeForAll uses unique combat TeamIds (roster slots) so FF-off never blocks tags;
		// palette still only has A/B — map via % 2 for a mixed look.
		Character->SetTeamColor(static_cast<uint8>(TeamId % 2));
	}
}

void APaintForgePlayerState::OnRep_Flags()
{
	// Mirror the replicated team onto the pawn's MID tint (covers flag-after-pawn ordering and
	// host team-cycles recoloring an already-spawned pawn) before notifying UI listeners.
	ApplyTeamColorToPawn(GetPawn());
	OnFlagsChangedEvent.Broadcast();
}
