// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/CombatForgeGameState.h"

#include "CombatForge.h"
#include "Core/CombatForgePlayerState.h"
#include "Net/UnrealNetwork.h"

namespace
{
	constexpr int32 ElimFeedCap = 50;   // §4.3 cosmetics: elim feed cap 50 entries
}

ACombatForgeGameState::ACombatForgeGameState()
{
	// GameState replicates at 10 Hz (T28); phase/score state is low-churn end-timestamp data.
	SetNetUpdateFrequency(10.f);
}

void ACombatForgeGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ACombatForgeGameState, Phase);
	DOREPLIFETIME(ACombatForgeGameState, PhaseEndServerTime);
	DOREPLIFETIME(ACombatForgeGameState, PhaseDuration);
	DOREPLIFETIME(ACombatForgeGameState, RoundState);
	DOREPLIFETIME(ACombatForgeGameState, RoundStateEndServerTime);
	DOREPLIFETIME(ACombatForgeGameState, RoundNumber);
	DOREPLIFETIME(ACombatForgeGameState, TeamRoundWins);
	DOREPLIFETIME(ACombatForgeGameState, AliveCounts);
	DOREPLIFETIME(ACombatForgeGameState, bSuddenDeath);
	DOREPLIFETIME(ACombatForgeGameState, ElimFeed);
	DOREPLIFETIME(ACombatForgeGameState, VoteTally);
	DOREPLIFETIME(ACombatForgeGameState, MatchId);
	DOREPLIFETIME(ACombatForgeGameState, TargetTeamSize);
	DOREPLIFETIME(ACombatForgeGameState, bFillWithBots);
	DOREPLIFETIME(ACombatForgeGameState, RoundWinsToTake);
	DOREPLIFETIME(ACombatForgeGameState, BuildMode);
	DOREPLIFETIME(ACombatForgeGameState, MatchType);
	DOREPLIFETIME(ACombatForgeGameState, ArenaMap);
	DOREPLIFETIME(ACombatForgeGameState, SelectedCommunityMapFile);
	DOREPLIFETIME(ACombatForgeGameState, SelectedCommunityMapLabel);
	DOREPLIFETIME(ACombatForgeGameState, TeamScores);
	DOREPLIFETIME(ACombatForgeGameState, CommunityBasePieces);
}

// ---------------------------------------------------------------------------
// Client-safe helpers
// ---------------------------------------------------------------------------

float ACombatForgeGameState::GetPhaseTimeRemaining() const
{
	if (PhaseEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, PhaseEndServerTime - GetServerWorldTimeSeconds());
}

float ACombatForgeGameState::GetRoundTimeRemaining() const
{
	if (RoundStateEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, RoundStateEndServerTime - GetServerWorldTimeSeconds());
}

bool ACombatForgeGameState::IsFireAllowed() const
{
	// T21: warm-up pen fire in Lobby, and Live combat rounds. Everything else rejects.
	return Phase == EPFMatchPhase::Lobby
		|| (Phase == EPFMatchPhase::Combat && RoundState == EPFRoundState::Live);
}

bool ACombatForgeGameState::IsBuildAllowed() const
{
	return Phase == EPFMatchPhase::Build;
}

ACombatForgePlayerState* ACombatForgeGameState::FindPlayerByRosterIndex(uint8 RosterIndex) const
{
	for (APlayerState* PS : PlayerArray)
	{
		ACombatForgePlayerState* PFPS = Cast<ACombatForgePlayerState>(PS);
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

void ACombatForgeGameState::ServerSetPhase(EPFMatchPhase NewPhase, float EndServerTime)
{
	if (!HasAuthority())
	{
		return;
	}
	Phase = NewPhase;
	PhaseEndServerTime = EndServerTime;
	// Stamp full-scale duration at phase entry so UI rings (vote) normalize against the real
	// GameMode config, not a hardcoded 20 s constant. Untimed phases (Lobby end=0) → 0.
	if (EndServerTime > 0.f)
	{
		PhaseDuration = FMath::Max(0.f, EndServerTime - GetServerWorldTimeSeconds());
	}
	else
	{
		PhaseDuration = 0.f;
	}
	OnRep_Phase();
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetRoundState(EPFRoundState NewState, float EndServerTime)
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

void ACombatForgeGameState::ServerSetPhaseEndTime(float EndServerTime)
{
	if (!HasAuthority())
	{
		return;
	}
	PhaseEndServerTime = EndServerTime;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetRoundNumber(uint8 NewRoundNumber)
{
	if (!HasAuthority())
	{
		return;
	}
	RoundNumber = NewRoundNumber;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetTeamRoundWins(uint8 WinsA, uint8 WinsB)
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

void ACombatForgeGameState::ServerSetTeamScores(uint16 ScoreA, uint16 ScoreB)
{
	if (!HasAuthority())
	{
		return;
	}
	TeamScores[0] = ScoreA;
	TeamScores[1] = ScoreB;
	OnRep_Score();       // same broadcast the HUD already binds (OnMatchScoreChangedEvent)
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetAliveCounts(uint8 AliveA, uint8 AliveB)
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

void ACombatForgeGameState::ServerAddElimEntry(const FPFElimEntry& Entry)
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

void ACombatForgeGameState::ServerSetVoteTally(const FPFVoteTally& NewTally)
{
	if (!HasAuthority())
	{
		return;
	}
	VoteTally = NewTally;
	OnRep_VoteTally();
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetSuddenDeath(bool bNewSuddenDeath)
{
	if (!HasAuthority())
	{
		return;
	}
	bSuddenDeath = bNewSuddenDeath;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetMatchId(const FString& NewMatchId)
{
	if (!HasAuthority())
	{
		return;
	}
	MatchId = NewMatchId;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetTargetTeamSize(uint8 NewSize)
{
	if (!HasAuthority())
	{
		return;
	}
	TargetTeamSize = static_cast<uint8>(FMath::Clamp<int32>(NewSize, 1, PFGrid::SpawnPointsPerTeam));
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetFillWithBots(bool bFill)
{
	if (!HasAuthority())
	{
		return;
	}
	bFillWithBots = bFill;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetRoundWinsToTake(uint8 NewWins)
{
	if (!HasAuthority())
	{
		return;
	}
	RoundWinsToTake = NewWins;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetBuildMode(EPFBuildMode NewMode)
{
	if (!HasAuthority())
	{
		return;
	}
	BuildMode = NewMode;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetMatchType(EPFMatchType NewType)
{
	if (!HasAuthority())
	{
		return;
	}
	MatchType = NewType;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetArenaMap(EPFArenaMap NewMap)
{
	if (!HasAuthority())
	{
		return;
	}
	ArenaMap = NewMap;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetSelectedCommunityMap(const FString& FileName, const FString& Label)
{
	if (!HasAuthority())
	{
		return;
	}
	// Basename only — reject path traversal.
	FString Clean = FileName;
	Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (Clean.Contains(TEXT("..")) || Clean.Contains(TEXT("/")))
	{
		Clean.Reset();
	}
	SelectedCommunityMapFile = Clean;
	SelectedCommunityMapLabel = Label.Left(120);
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerSetCommunityBasePieces(uint16 Count)
{
	if (!HasAuthority())
	{
		return;
	}
	CommunityBasePieces = Count;
	ForceNetUpdate();
}

void ACombatForgeGameState::ServerResetMatchState()
{
	if (!HasAuthority())
	{
		return;
	}
	RoundNumber = 0;
	bSuddenDeath = false;
	TeamRoundWins[0] = 0;
	TeamRoundWins[1] = 0;
	TeamScores[0] = 0;
	TeamScores[1] = 0;
	AliveCounts[0] = 0;
	AliveCounts[1] = 0;
	CommunityBasePieces = 0;
	ElimFeed.Reset();
	VoteTally = FPFVoteTally();
	const bool bRoundChanged = (RoundState != EPFRoundState::None);
	RoundState = EPFRoundState::None;
	RoundStateEndServerTime = 0.f;
	OnRep_Score();
	OnRep_AliveCounts();
	OnRep_ElimFeed();
	OnRep_VoteTally();
	if (bRoundChanged)
	{
		OnRep_RoundState();   // listen-host broadcast (was missing — host-only UI/input edge)
	}
	ForceNetUpdate();
}

// ---------------------------------------------------------------------------
// OnReps — must be single-shot tolerant (02 R4): each one produces correct
// results when called once with final state (join-in-progress).
// ---------------------------------------------------------------------------

void ACombatForgeGameState::OnRep_Phase()
{
	UE_LOG(CombatForgeLog, Log, TEXT("GameState: phase -> %d"), static_cast<int32>(Phase));
	OnPhaseChangedEvent.Broadcast(Phase);
}

void ACombatForgeGameState::OnRep_RoundState()
{
	OnRoundStateChangedEvent.Broadcast(RoundState);
}

void ACombatForgeGameState::OnRep_Score()
{
	OnMatchScoreChangedEvent.Broadcast();
}

void ACombatForgeGameState::OnRep_AliveCounts()
{
	OnAliveCountsChangedEvent.Broadcast();
}

void ACombatForgeGameState::OnRep_ElimFeed()
{
	OnElimFeedChangedEvent.Broadcast();
}

void ACombatForgeGameState::OnRep_VoteTally()
{
	OnVoteTallyChangedEvent.Broadcast();
}
