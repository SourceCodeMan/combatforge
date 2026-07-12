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
	DOREPLIFETIME(APaintForgeGameState, PhaseDuration);
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
	DOREPLIFETIME(APaintForgeGameState, bFillWithBots);
	DOREPLIFETIME(APaintForgeGameState, RoundWinsToTake);
	DOREPLIFETIME(APaintForgeGameState, BuildMode);
	DOREPLIFETIME(APaintForgeGameState, MatchType);
	DOREPLIFETIME(APaintForgeGameState, SelectedCommunityMapFile);
	DOREPLIFETIME(APaintForgeGameState, SelectedCommunityMapLabel);
	DOREPLIFETIME(APaintForgeGameState, TeamScores);
	DOREPLIFETIME(APaintForgeGameState, CommunityBasePieces);
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

void APaintForgeGameState::ServerSetTeamScores(uint16 ScoreA, uint16 ScoreB)
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

void APaintForgeGameState::ServerSetFillWithBots(bool bFill)
{
	if (!HasAuthority())
	{
		return;
	}
	bFillWithBots = bFill;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetRoundWinsToTake(uint8 NewWins)
{
	if (!HasAuthority())
	{
		return;
	}
	RoundWinsToTake = NewWins;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetBuildMode(EPFBuildMode NewMode)
{
	if (!HasAuthority())
	{
		return;
	}
	BuildMode = NewMode;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetMatchType(EPFMatchType NewType)
{
	if (!HasAuthority())
	{
		return;
	}
	MatchType = NewType;
	ForceNetUpdate();
}

void APaintForgeGameState::ServerSetSelectedCommunityMap(const FString& FileName, const FString& Label)
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

void APaintForgeGameState::ServerSetCommunityBasePieces(uint16 Count)
{
	if (!HasAuthority())
	{
		return;
	}
	CommunityBasePieces = Count;
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
