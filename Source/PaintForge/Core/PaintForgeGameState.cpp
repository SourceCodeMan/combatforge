// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PaintForgeGameState.h"

#include "PaintForge.h"
#include "Core/PaintForgePlayerState.h"
#include "Net/UnrealNetwork.h"

namespace
{
	constexpr int32 ElimFeedCap = 50;   // §4.3 cosmetics: elim feed cap 50 entries
}

APaintForgeGameState::APaintForgeGameState()
{
	// GameState replicates at 10 Hz (T28); phase/score state is low-churn end-timestamp data.
	SetNetUpdateFrequency(10.f);
}

void APaintForgeGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(APaintForgeGameState, Phase);
	DOREPLIFETIME(APaintForgeGameState, PhaseEndServerTime);
	DOREPLIFETIME(APaintForgeGameState, RoundState);
	DOREPLIFETIME(APaintForgeGameState, RoundStateEndServerTime);
	DOREPLIFETIME(APaintForgeGameState, RoundNumber);
	DOREPLIFETIME(APaintForgeGameState, TeamRoundWins);
	DOREPLIFETIME(APaintForgeGameState, AliveCounts);
	DOREPLIFETIME(APaintForgeGameState, bSuddenDeath);
	DOREPLIFETIME(APaintForgeGameState, ElimFeed);
	DOREPLIFETIME(APaintForgeGameState, VoteTally);
	DOREPLIFETIME(APaintForgeGameState, MatchId);
	DOREPLIFETIME(APaintForgeGameState, TargetTeamSize);
}

// ---------------------------------------------------------------------------
// Client-safe helpers
// ---------------------------------------------------------------------------

float APaintForgeGameState::GetPhaseTimeRemaining() const
{
	if (PhaseEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, PhaseEndServerTime - GetServerWorldTimeSeconds());
}

float APaintForgeGameState::GetRoundTimeRemaining() const
{
	if (RoundStateEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, RoundStateEndServerTime - GetServerWorldTimeSeconds());
}

bool APaintForgeGameState::IsFireAllowed() const
{
	// T21: warm-up pen fire in Lobby, and Live combat rounds. Everything else rejects.
	return Phase == EPFMatchPhase::Lobby
		|| (Phase == EPFMatchPhase::Combat && RoundState == EPFRoundState::Live);
}

bool APaintForgeGameState::IsBuildAllowed() const
{
	return Phase == EPFMatchPhase::Build;
}

APaintForgePlayerState* APaintForgeGameState::FindPlayerByRosterIndex(uint8 RosterIndex) const
{
	for (APlayerState* PS : PlayerArray)
	{
		APaintForgePlayerState* PFPS = Cast<APaintForgePlayerState>(PS);
		if (PFPS && PFPS->RosterIndex == RosterIndex)
		{
			return PFPS;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Server setters — set, then manually invoke the OnRep so the listen host
// broadcasts too (OnReps never auto-fire where the property is written — R9).
// ---------------------------------------------------------------------------

void APaintForgeGameState::ServerSetPhase(EPFMatchPhase NewPhase, float EndServerTime)
{
	if (!HasAuthority())
	{
		return;
	}
	Phase = NewPhase;
	PhaseEndServerTime = EndServerTime;
	OnRep_Phase();
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetRoundState(EPFRoundState NewState, float EndServerTime)
{
	if (!HasAuthority())
	{
		return;
	}
	RoundState = NewState;
	RoundStateEndServerTime = EndServerTime;
	OnRep_RoundState();
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetPhaseEndTime(float EndServerTime)
{
	if (!HasAuthority())
	{
		return;
	}
	PhaseEndServerTime = EndServerTime;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetRoundNumber(uint8 NewRoundNumber)
{
	if (!HasAuthority())
	{
		return;
	}
	RoundNumber = NewRoundNumber;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetTeamRoundWins(uint8 WinsA, uint8 WinsB)
{
	if (!HasAuthority())
	{
		return;
	}
	TeamRoundWins[0] = WinsA;
	TeamRoundWins[1] = WinsB;
	OnRep_Score();
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetAliveCounts(uint8 AliveA, uint8 AliveB)
{
	if (!HasAuthority())
	{
		return;
	}
	if (AliveCounts[0] == AliveA && AliveCounts[1] == AliveB)
	{
		return;
	}
	AliveCounts[0] = AliveA;
	AliveCounts[1] = AliveB;
	OnRep_AliveCounts();
	ForceNetUpdate();
}

void APaintForgeGameState::ServerAddElimEntry(const FPFElimEntry& Entry)
{
	if (!HasAuthority())
	{
		return;
	}
	ElimFeed.Add(Entry);
	while (ElimFeed.Num() > ElimFeedCap)
	{
		ElimFeed.RemoveAt(0);
	}
	OnRep_ElimFeed();
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetVoteTally(const FPFVoteTally& NewTally)
{
	if (!HasAuthority())
	{
		return;
	}
	VoteTally = NewTally;
	OnRep_VoteTally();
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetSuddenDeath(bool bNewSuddenDeath)
{
	if (!HasAuthority())
	{
		return;
	}
	bSuddenDeath = bNewSuddenDeath;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetMatchId(const FString& NewMatchId)
{
	if (!HasAuthority())
	{
		return;
	}
	MatchId = NewMatchId;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetTargetTeamSize(uint8 NewSize)
{
	if (!HasAuthority())
	{
		return;
	}
	TargetTeamSize = static_cast<uint8>(FMath::Clamp<int32>(NewSize, 1, PFGrid::SpawnPointsPerTeam));
	ForceNetUpdate();
}

void APaintForgeGameState::ServerResetMatchState()
{
	if (!HasAuthority())
	{
		return;
	}
	RoundNumber = 0;
	bSuddenDeath = false;
	TeamRoundWins[0] = 0;
	TeamRoundWins[1] = 0;
	AliveCounts[0] = 0;
	AliveCounts[1] = 0;
	ElimFeed.Reset();
	VoteTally = FPFVoteTally();
	RoundState = EPFRoundState::None;
	RoundStateEndServerTime = 0.f;
	OnRep_Score();
	OnRep_AliveCounts();
	OnRep_ElimFeed();
	OnRep_VoteTally();
	ForceNetUpdate();
}

// ---------------------------------------------------------------------------
// OnReps — must be single-shot tolerant (02 R4): each one produces correct
// results when called once with final state (join-in-progress).
// ---------------------------------------------------------------------------

void APaintForgeGameState::OnRep_Phase()
{
	UE_LOG(PaintForgeLog, Log, TEXT("GameState: phase -> %d"), static_cast<int32>(Phase));
	OnPhaseChangedEvent.Broadcast(Phase);
}

void APaintForgeGameState::OnRep_RoundState()
{
	OnRoundStateChangedEvent.Broadcast(RoundState);
}

void APaintForgeGameState::OnRep_Score()
{
	OnMatchScoreChangedEvent.Broadcast();
}

void APaintForgeGameState::OnRep_AliveCounts()
{
	OnAliveCountsChangedEvent.Broadcast();
}

void APaintForgeGameState::OnRep_ElimFeed()
{
	OnElimFeedChangedEvent.Broadcast();
}

void APaintForgeGameState::OnRep_VoteTally()
{
	OnVoteTallyChangedEvent.Broadcast();
}
