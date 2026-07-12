// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PaintForgeGameMode.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerController.h"
#include "Core/PaintForgePlayerState.h"
#include "Player/PaintForgeCharacter.h"
#include "AI/PFBotController.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFTargetDummy.h"
#include "Building/PFArenaShell.h"
#include "Building/PFBuildGrid.h"
#include "Voting/PFRatingSubsystem.h"

#include "GameFramework/PawnMovementComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/Guid.h"
#include "TimerManager.h"

namespace
{
	// T18 scoring values.
	constexpr int32 ScoreElimination  = 100;
	constexpr int32 ScoreRoundSurvive = 25;
	constexpr int32 ScoreRoundWin     = 50;

	// B6 budgets, reset each Lobby→Build.
	constexpr uint8 BudgetStructural = 30;
	constexpr uint8 BudgetProps      = 6;

	// T15 small-format scaling (≤2v2).
	constexpr uint8 SmallRoundWinsToTake = 3;
	constexpr uint8 SmallMaxRounds       = 5;
	constexpr float SmallRoundDuration   = 60.f;

	constexpr uint8 TeamNone = 255;
}

APaintForgeGameMode::APaintForgeGameMode()
{
	DefaultPawnClass      = APaintForgeCharacter::StaticClass();
	PlayerControllerClass = APaintForgePlayerController::StaticClass();
	GameStateClass        = APaintForgeGameState::StaticClass();
	PlayerStateClass      = APaintForgePlayerState::StaticClass();
	bUseSeamlessTravel = false;   // single persistent level, no travel (02 D1)
}

// ---------------------------------------------------------------------------
// World bootstrap
// ---------------------------------------------------------------------------

void APaintForgeGameMode::BeginPlay()
{
	Super::BeginPlay();
	SpawnArenaActors();

	EffectiveRoundWinsToTake = RoundWinsToTakeMatch;
	EffectiveMaxRounds = MaxRounds;
	EffectiveRoundDuration = RoundDuration;

	if (APaintForgeGameState* GS = GetPFGameState())
	{
		GS->ServerSetTargetTeamSize(DefaultTeamSize);   // 4v4 default; host can switch to 6v6 in Lobby
		GS->ServerSetBuildMode(DefaultBuildMode);       // Creative default; host picks in Lobby / menu
		GS->ServerSetMatchType(DefaultMatchType);       // Elimination default
	}
}

void APaintForgeGameMode::SpawnArenaActors()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Arena shell (floor slab, perimeter, midline barrier, spawn strips, warm-up pen) and the
	// single replicated build-grid container (B8). Both replicate; geometry is ctor-built.
	ArenaShell = World->SpawnActor<APFArenaShell>(APFArenaShell::StaticClass(), FTransform::Identity, Params);
	BuildGrid  = World->SpawnActor<APFBuildGrid>(APFBuildGrid::StaticClass(), FTransform::Identity, Params);

	// The lighting rig is spawned per-machine by UPFLightingSubsystem (host AND every remote client) so
	// clients aren't left with an unlit scene — the server-only GameMode must not own render-only actors.

	if (AWorldSettings* WorldSettings = World->GetWorldSettings())
	{
		WorldSettings->KillZ = -1000.f;
	}

	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: arena shell and build grid spawned"));
}

// ---------------------------------------------------------------------------
// Login / logout / spawning
// ---------------------------------------------------------------------------

void APaintForgeGameMode::PostLogin(APlayerController* NewPlayer)
{
	// Assign team + roster slot BEFORE Super::PostLogin so the initial pawn spawn already
	// knows its side (RestartPlayer → GetSpawnTransform reads TeamId).
	APaintForgePlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (PS && PS->TeamId == TeamNone)
	{
		int32 TeamA = 0, TeamB = 0;
		GetTeamCounts(TeamA, TeamB);
		// Balance on HUMAN counts, not bot-padded totals — otherwise every mid-match joiner stacks on
		// team 0 (bots keep the totals equal, so the tie always resolves to 0).
		const int32 HumansA = TeamA - GetTeamCountByKind(0, /*bBotsOnly=*/true);
		const int32 HumansB = TeamB - GetTeamCountByKind(1, /*bBotsOnly=*/true);
		const uint8 NewTeam = (HumansB < HumansA) ? 1 : 0;   // fewer real players; tie → A
		PS->ServerSetTeam(NewTeam, FindFreeRosterIndex());
	}

	Super::PostLogin(NewPlayer);

	if (PS)
	{
		SpawnWarmupDummyFor(PS);   // one pen dummy per connected player (T29)

		// Joiners during Combat enter the current round alive at their team spawn.
		// CONTRACT-GAP: contract is silent on mid-round joiners; alive-at-spawn is the smallest
		// implementation that keeps alive counts and elim-victory checks self-consistent.
		// Keep the joined team at/under the format size. Prefer to free a bot slot; if the team is all
		// humans (no bot to drop), move the joiner to the other side when it has room, so no team
		// exceeds the format (which would also alias onto the 6 fixed spawn slots).
		if (bFillWithBots && GetPFGameState() && PS->TeamId <= 1 &&
			GetTeamCountByKind(PS->TeamId, /*bBotsOnly=*/false) > GetPFGameState()->TargetTeamSize)
		{
			if (GetTeamCountByKind(PS->TeamId, /*bBotsOnly=*/true) > 0)
			{
				TrimOneBotFromTeam(PS->TeamId);
			}
			else
			{
				const uint8 Other = static_cast<uint8>(1 - PS->TeamId);
				if (GetTeamCountByKind(Other, /*bBotsOnly=*/false) < GetPFGameState()->TargetTeamSize)
				{
					PS->ServerSetTeam(Other, PS->RosterIndex);
				}
			}
		}

		if (GetPFGameState() && GetPFGameState()->Phase == EPFMatchPhase::Combat)
		{
			PS->bAliveInRound = (GetPFGameState()->RoundState == EPFRoundState::Freeze ||
			                     GetPFGameState()->RoundState == EPFRoundState::Live);
		}
		RecountAlive();

		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: %s joined (team %d, roster %d)"),
			*PS->GetPlayerName(), PS->TeamId, PS->RosterIndex);
	}
}

void APaintForgeGameMode::Logout(AController* Exiting)
{
	APaintForgePlayerState* ExitingPS =
		Exiting ? Exiting->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (ExitingPS)
	{
		// The leaver's PlayerState may linger in PlayerArray briefly; take them out of the
		// alive count NOW so the post-logout victory check below is correct.
		ExitingPS->bAliveInRound = false;
		if (TObjectPtr<APFTargetDummy>* Dummy = WarmupDummies.Find(ExitingPS))
		{
			if (*Dummy)
			{
				(*Dummy)->Destroy();
			}
			WarmupDummies.Remove(ExitingPS);
		}
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: %s left"), *ExitingPS->GetPlayerName());
	}

	Super::Logout(Exiting);

	// A leaver can complete an elimination victory, an all-ready condition, or an all-voted
	// condition. Their PlayerState may still sit in PlayerArray here, so the ready/vote scans
	// take it as an explicit exclusion.
	RecountAlive();
	CheckElimVictory();
	CheckSkirmishAbandon();   // Skirmish: a whole team leaving ends the match (CheckElimVictory no-ops here)
	NotifyReadyChangedInternal(ExitingPS);
	CheckAllVotesIn(ExitingPS);
}

void APaintForgeGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer || NewPlayer->GetPawn())
	{
		return;
	}
	const APaintForgePlayerState* PS = NewPlayer->GetPlayerState<APaintForgePlayerState>();
	if (!PS)
	{
		return;
	}

	RestartPlayerAtTransform(NewPlayer, GetSpawnTransform(PS));

	if (APaintForgeCharacter* Pawn = Cast<APaintForgeCharacter>(NewPlayer->GetPawn()))
	{
		Pawn->SetTeamColor(PS->TeamId);

		// §3.4: GameMode subscribes to the pawn's elimination broadcast (the health component
		// never calls up into the GameMode). RemoveAll first keeps re-spawns single-bound.
		if (UPFHealthComponent* Health = Pawn->GetHealth())
		{
			Health->OnEliminatedEvent.RemoveAll(this);
			Health->OnEliminatedEvent.AddUObject(this, &APaintForgeGameMode::HandlePawnHealthEliminated);
		}
	}
}

void APaintForgeGameMode::HandlePawnHealthEliminated(UPFHealthComponent* Health, const FPFPaintHitInfo& FinalHit)
{
	APaintForgeCharacter* Victim = Health ? Cast<APaintForgeCharacter>(Health->GetOwner()) : nullptr;
	if (Victim)
	{
		NotifyPawnEliminated(Victim, FinalHit);
	}
}

void APaintForgeGameMode::SpawnWarmupDummyFor(APaintForgePlayerState* PS)
{
	if (!PS || !ArenaShell || WarmupDummies.Contains(PS))
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FTransform Slot = ArenaShell->GetWarmupDummyTransform(
		FMath::Min<int32>(PS->RosterIndex, PFGrid::MaxRosterSlots - 1));

	if (APFTargetDummy* Dummy = World->SpawnActor<APFTargetDummy>(APFTargetDummy::StaticClass(), Slot, Params))
	{
		WarmupDummies.Add(PS, Dummy);
	}
}

// ---------------------------------------------------------------------------
// Spawn transforms (side swap: odd rounds = home half — B1)
// ---------------------------------------------------------------------------

FTransform APaintForgeGameMode::GetSpawnTransform(const APaintForgePlayerState* PS) const
{
	if (!PS || !ArenaShell)
	{
		return FTransform(FVector(0.f, 0.f, 200.f));
	}

	const APaintForgeGameState* GS = GetPFGameState();
	const EPFMatchPhase Phase = GS ? GS->Phase : EPFMatchPhase::Lobby;
	const uint8 Team = (PS->TeamId <= 1) ? PS->TeamId : 0;
	const int32 TeamSlot = GetTeamSlotIndex(PS);

	switch (Phase)
	{
	case EPFMatchPhase::Lobby:
		return ArenaShell->GetWarmupSpawnTransform(FMath::Min<int32>(PS->RosterIndex, PFGrid::MaxRosterSlots - 1));

	case EPFMatchPhase::Build:
		return ArenaShell->GetBuildStartTransform(Team, TeamSlot);

	case EPFMatchPhase::Combat:
	case EPFMatchPhase::Vote:
	case EPFMatchPhase::Results:
	default:
	{
		// Odd rounds: home half (team 0 on side 0); even rounds swapped. Round 0 counts as 1.
		const uint8 Round = (GS && GS->RoundNumber > 0) ? GS->RoundNumber : 1;
		const uint8 PhysicalSide = (Round % 2 == 1) ? Team : (1 - Team);
		return ArenaShell->GetTeamSpawnTransform(PhysicalSide, TeamSlot % PFGrid::SpawnPointsPerTeam);
	}
	}
}

int32 APaintForgeGameMode::GetTeamSlotIndex(const APaintForgePlayerState* PS) const
{
	// Ordinal among same-team players sorted by RosterIndex — deterministic and index-stable.
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS || !PS)
	{
		return 0;
	}
	int32 Slot = 0;
	for (APlayerState* Other : GS->PlayerArray)
	{
		const APaintForgePlayerState* OtherPS = Cast<APaintForgePlayerState>(Other);
		if (OtherPS && OtherPS != PS && OtherPS->TeamId == PS->TeamId &&
			OtherPS->RosterIndex < PS->RosterIndex)
		{
			++Slot;
		}
	}
	return Slot;
}

// ---------------------------------------------------------------------------
// THE phase mutator
// ---------------------------------------------------------------------------

void APaintForgeGameMode::SetPhase(EPFMatchPhase NewPhase)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || !HasAuthority())
	{
		return;
	}
	const EPFMatchPhase OldPhase = GS->Phase;

	// Every transition cancels pending phase machinery.
	GetWorldTimerManager().ClearTimer(PhaseTimerHandle);
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	GetWorldTimerManager().ClearTimer(LobbyCountdownHandle);
	bLobbyCountdownActive = false;
	bLobbyCountdownForced = false;
	bBuildEarlyEndActive = false;

	const float Now = GS->GetServerWorldTimeSeconds();
	float EndTime = 0.f;

	switch (NewPhase)
	{
	case EPFMatchPhase::Lobby:   EndTime = 0.f; break;                        // untimed
	case EPFMatchPhase::Build:   EndTime = Now + BuildPhaseDuration; break;
	case EPFMatchPhase::Combat:  EndTime = 0.f; break;                        // round timers drive
	case EPFMatchPhase::Vote:    EndTime = Now + VotePhaseDuration; break;
	case EPFMatchPhase::Results: EndTime = Now + ResultsDuration; break;
	}

	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: phase %d -> %d"),
		static_cast<int32>(OldPhase), static_cast<int32>(NewPhase));
	GS->ServerSetPhase(NewPhase, EndTime);

	switch (NewPhase)
	{
	case EPFMatchPhase::Lobby:
	{
		// Results→Lobby: same human roster kept; bots are dropped so the next match re-fills fresh.
		GS->ServerSetRoundState(EPFRoundState::None, 0.f);
		RemoveAllBots();
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
			if (!PS)
			{
				continue;
			}
			PS->ServerSetReady(false);
			PS->bAliveInRound = true;
			if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(PS->GetPlayerController()))
			{
				PC->SetEliminatedMoveLock(false);
			}
			RespawnCombatant(PS, 3);   // players and bots alike
		}
		RecountAlive();
		break;
	}

	case EPFMatchPhase::Build:
	{
		// New match record identity + full match-state reset (Lobby→Build side effects).
		MatchStartServerTime = Now;
		BuildPhaseFullEndTime = EndTime;
		GS->ServerResetMatchState();
		GS->ServerSetMatchId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower());
		ComputeEffectiveScaling();
		ResetPlayerMatchStats();

		bSuddenDeathPlayed = false;
		bPendingSuddenDeath = false;
		bPendingMatchOver = false;
		PendingMatchWinner = TeamNone;
		PendingMatchResult = FPFMatchResult();

		if (BuildGrid)
		{
			BuildGrid->ClearAll();
		}
		if (ArenaShell)
		{
			ArenaShell->SetMidlineBarrierActive(true);
		}
		// Splat-pool reset happens client-side from the phase delegate (T20).

		// Fill both teams up to the selected format with bots BEFORE the reset loop, so the bots are in
		// PlayerArray and get spawned/positioned by the same loop as the humans.
		FillBotsToFormat();

		for (APlayerState* PSBase : GS->PlayerArray)
		{
			APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
			if (!PS)
			{
				continue;
			}
			PS->ServerSetBudgets(BudgetStructural, BudgetProps);
			PS->ServerSetReady(false);
			PS->bAliveInRound = true;
			if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(PS->GetPlayerController()))
			{
				PC->SetEliminatedMoveLock(false);
			}
			RespawnCombatant(PS, 3);   // own plot; players and bots alike
		}
		RecountAlive();

		// All-bot team → auto-fill THAT team's half from a community-favorite arena (nobody is there to
		// build it). Injected here (after bots exist, grid cleared, before FreezeBuild) so it replicates
		// through the build window and is captured by BeginMatchRecord at Combat. Skipped in Play-only
		// (which flashes past the build phase).
		if (BuildGrid && GS->BuildMode != EPFBuildMode::PlayOnly)
		{
			if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
			{
				if (GS->BuildMode == EPFBuildMode::Improvement)
				{
					// Improvement: load the WHOLE community map; everyone builds on top of it.
					TArray<FPFBuildPieceRec> Whole;
					if (Rating->PickCommunityArena(Whole))
					{
						BuildGrid->ServerInjectPieces(Whole);
					}
				}
				else   // Creative: only an all-bot team's half needs filling
				{
					for (uint8 BotTeam = 0; BotTeam <= 1; ++BotTeam)
					{
						const int32 Total = GetTeamCountByKind(BotTeam, /*bBotsOnly=*/false);
						if (Total > 0 && Total == GetTeamCountByKind(BotTeam, /*bBotsOnly=*/true))
						{
							TArray<FPFBuildPieceRec> Half;
							if (Rating->PickCommunityHalf(Half, BotTeam))
							{
								BuildGrid->ServerInjectPieces(Half);
							}
						}
					}
				}
			}
		}

		// Play-only mode runs all the Build-phase SETUP above (match reset, bots, spawns) but skips the
		// build TIME — flash straight to Combat. (Creative/Improvement get the full build window.)
		const bool bPlayOnly = (GS->BuildMode == EPFBuildMode::PlayOnly);
		if (bPlayOnly)
		{
			GS->ServerSetPhaseEndTime(Now + 0.1f);
		}
		GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
			&APaintForgeGameMode::StartNextRoundFromBuildEnd,
			bPlayOnly ? 0.1f : BuildPhaseDuration, false);
		break;
	}

	case EPFMatchPhase::Combat:
	{
		// Build→Combat: freeze grid, record the arena, drop the midline barrier, round 1 Freeze.
		if (BuildGrid)
		{
			BuildGrid->FreezeBuild();
			if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
			{
				int32 TeamA = 0, TeamB = 0;
				GetTeamCounts(TeamA, TeamB);
				Rating->BeginMatchRecord(GS->MatchId, BuildGrid->GetPieces(), FMath::Max(TeamA, TeamB));
			}
		}
		if (ArenaShell)
		{
			ArenaShell->SetMidlineBarrierActive(false);
		}
		StartNextRound();
		break;
	}

	case EPFMatchPhase::Vote:
	{
		GS->ServerSetRoundState(EPFRoundState::None, 0.f);
		GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
			&APaintForgeGameMode::FinalizeVotePhase, VotePhaseDuration, false);
		break;
	}

	case EPFMatchPhase::Results:
	{
		// Vote→Results: the tally is already on GameState; commit the match record to disk.
		if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
		{
			Rating->CommitMatchRecord(PendingMatchResult);
		}
		GetWorldTimerManager().SetTimer(PhaseTimerHandle,
			FTimerDelegate::CreateUObject(this, &APaintForgeGameMode::SetPhase, EPFMatchPhase::Lobby),
			ResultsDuration, false);
		break;
	}
	}

	ApplyServerMoveLocks();
}

// ---------------------------------------------------------------------------
// Lobby / Build ready flow
// ---------------------------------------------------------------------------

void APaintForgeGameMode::NotifyReadyChanged()
{
	NotifyReadyChangedInternal(nullptr);
}

void APaintForgeGameMode::NotifyReadyChangedInternal(const APaintForgePlayerState* IgnorePS)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}

	// Connected-player count excluding a lingering leaver (Logout path).
	int32 Connected = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (PSBase && PSBase != IgnorePS)
		{
			++Connected;
		}
	}

	if (GS->Phase == EPFMatchPhase::Lobby)
	{
		// All-ready (2+ players) → 5 s countdown; a dropped ready cancels a non-forced countdown.
		const bool bAllReady = AreAllPlayersReady(IgnorePS) && Connected >= 2;
		if (bAllReady && !bLobbyCountdownActive)
		{
			BeginLobbyStartCountdown(/*bForced=*/false);
		}
		else if (!bAllReady && bLobbyCountdownActive && !bLobbyCountdownForced)
		{
			CancelLobbyStartCountdown();
		}
	}
	else if (GS->Phase == EPFMatchPhase::Build)
	{
		// Both teams 100% ready → 5 s countdown early end (§4.3); dropped ready restores the clock.
		const bool bAllReady = AreAllPlayersReady(IgnorePS) && Connected >= 1;
		const float Now = GS->GetServerWorldTimeSeconds();
		if (bAllReady && !bBuildEarlyEndActive)
		{
			const float Remaining = GS->GetPhaseTimeRemaining();
			if (Remaining > LobbyStartCountdown)
			{
				bBuildEarlyEndActive = true;
				GS->ServerSetPhaseEndTime(Now + LobbyStartCountdown);
				GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
					&APaintForgeGameMode::StartNextRoundFromBuildEnd, LobbyStartCountdown, false);
				UE_LOG(PaintForgeLog, Log, TEXT("GameMode: all ready - build phase ending in %.0f s"), LobbyStartCountdown);
			}
		}
		else if (!bAllReady && bBuildEarlyEndActive)
		{
			bBuildEarlyEndActive = false;
			const float Restored = FMath::Max(BuildPhaseFullEndTime - Now, 0.1f);
			GS->ServerSetPhaseEndTime(Now + Restored);
			GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
				&APaintForgeGameMode::StartNextRoundFromBuildEnd, Restored, false);
		}
	}
}

void APaintForgeGameMode::BeginLobbyStartCountdown(bool bForced)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || bLobbyCountdownActive)
	{
		return;
	}
	bLobbyCountdownActive = true;
	bLobbyCountdownForced = bForced;
	GS->ServerSetPhaseEndTime(GS->GetServerWorldTimeSeconds() + LobbyStartCountdown);
	GetWorldTimerManager().SetTimer(LobbyCountdownHandle,
		FTimerDelegate::CreateUObject(this, &APaintForgeGameMode::SetPhase, EPFMatchPhase::Build),
		LobbyStartCountdown, false);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: match starting in %.0f s (%s)"),
		LobbyStartCountdown, bForced ? TEXT("host force") : TEXT("all ready"));
}

void APaintForgeGameMode::CancelLobbyStartCountdown()
{
	if (!bLobbyCountdownActive)
	{
		return;
	}
	bLobbyCountdownActive = false;
	bLobbyCountdownForced = false;
	GetWorldTimerManager().ClearTimer(LobbyCountdownHandle);
	if (APaintForgeGameState* GS = GetPFGameState())
	{
		GS->ServerSetPhaseEndTime(0.f);
	}
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: lobby start countdown cancelled"));
}

void APaintForgeGameMode::HostForceStart()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;
	}
	BeginLobbyStartCountdown(/*bForced=*/true);
}

void APaintForgeGameMode::HostCycleTeam(APaintForgePlayerState* Target)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || !Target)
	{
		return;
	}
	const uint8 NewTeam = (Target->TeamId == 0) ? 1 : 0;
	// Don't overstack a side beyond the format size (also protects the 6 fixed spawn slots per team).
	if (GetTeamCountByKind(NewTeam, /*bBotsOnly=*/false) >= GS->TargetTeamSize)
	{
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: host cycle refused - team %d already at format size"), NewTeam);
		return;
	}
	Target->ServerSetTeam(NewTeam, Target->RosterIndex);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: host moved %s to team %d"), *Target->GetPlayerName(), NewTeam);
}

void APaintForgeGameMode::HostReturnToLobby()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Results)
	{
		return;
	}
	SetPhase(EPFMatchPhase::Lobby);
}

void APaintForgeGameMode::HostSetFormat(uint8 NewTeamSize)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;   // format locks once the match starts (bots + scaling resolve at Lobby→Build)
	}
	GS->ServerSetTargetTeamSize(NewTeamSize);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: host set format to %dv%d"),
		GS->TargetTeamSize, GS->TargetTeamSize);
}

void APaintForgeGameMode::HostSetBuildMode(EPFBuildMode NewMode)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || NewMode >= EPFBuildMode::MAX_Count)
	{
		return;
	}
	GS->ServerSetBuildMode(NewMode);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: host set build mode %d"), static_cast<int32>(NewMode));
}

void APaintForgeGameMode::HostSetMatchType(EPFMatchType NewType)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || NewType >= EPFMatchType::MAX_Count)
	{
		return;
	}
	GS->ServerSetMatchType(NewType);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: host set match type %d"), static_cast<int32>(NewType));
}

// ---------------------------------------------------------------------------
// Round loop
// ---------------------------------------------------------------------------

void APaintForgeGameMode::StartNextRoundFromBuildEnd()
{
	// Build timer / early-end countdown fired: unspent budget is discarded implicitly.
	SetPhase(EPFMatchPhase::Combat);
}

void APaintForgeGameMode::StartNextRound()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;
	}

	bSuddenDeathRoundActive = bPendingSuddenDeath;
	bPendingSuddenDeath = false;
	if (bSuddenDeathRoundActive)
	{
		bSuddenDeathPlayed = true;
		GS->ServerSetSuddenDeath(true);
	}

	GS->ServerSetRoundNumber(GS->RoundNumber + 1);
	const uint8 RoundHP = bSuddenDeathRoundActive ? 1 : 3;   // B2; sudden death is a parameter, not a system

	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		PS->bAliveInRound = true;
		if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(PS->GetPlayerController()))
		{
			PC->SetEliminatedMoveLock(false);   // back alive; freeze-state lock reapplies below
		}
		RespawnCombatant(PS, RoundHP);          // players and bots alike
		if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(PS->GetPlayerController()))
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
		}
	}
	RecountAlive();

	GS->ServerSetRoundState(EPFRoundState::Freeze, GS->GetServerWorldTimeSeconds() + FreezeDuration);
	ApplyServerMoveLocks();
	GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
		&APaintForgeGameMode::BeginLiveRound, FreezeDuration, false);

	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: round %d freeze (%s)"),
		GS->RoundNumber, bSuddenDeathRoundActive ? TEXT("SUDDEN DEATH") : TEXT("normal"));
}

void APaintForgeGameMode::ResetPawnForRound(APaintForgeCharacter* Pawn, APaintForgePlayerState* PS, uint8 RoundHP)
{
	if (!Pawn || !PS)
	{
		return;
	}
	Pawn->GetHealth()->ResetForRound(RoundHP);   // restores HP, collision, appearance
	TeleportPawnTo(Pawn, GetSpawnTransform(PS));
}

void APaintForgeGameMode::TeleportPawnTo(APaintForgeCharacter* Pawn, const FTransform& Transform)
{
	if (!Pawn)
	{
		return;
	}
	Pawn->TeleportTo(Transform.GetLocation(), Transform.GetRotation().Rotator(),
		/*bIsATest=*/false, /*bNoCheck=*/true);
	if (UPawnMovementComponent* Move = Pawn->GetMovementComponent())
	{
		Move->StopMovementImmediately();
	}
	if (AController* Controller = Pawn->GetController())
	{
		Controller->SetControlRotation(Transform.GetRotation().Rotator());
	}
}

void APaintForgeGameMode::RespawnCombatant(APaintForgePlayerState* PS, uint8 RoundHP)
{
	// Controller-agnostic: works for a human PlayerController AND a bot AIController. The pawn is found
	// via the PlayerState (PS->GetPawn), and a missing pawn is restarted via PS->GetOwningController so
	// bots go through the same RestartPlayer path humans do (which also binds their elimination event).
	if (!PS)
	{
		return;
	}
	if (APaintForgeCharacter* Pawn = Cast<APaintForgeCharacter>(PS->GetPawn()))
	{
		if (UPFHealthComponent* Health = Pawn->GetHealth())
		{
			Health->ResetForRound(RoundHP);   // restores HP, collision, appearance
		}
		TeleportPawnTo(Pawn, GetSpawnTransform(PS));
	}
	else if (AController* Ctrl = PS->GetOwningController())
	{
		RestartPlayer(Ctrl);
		if (APaintForgeCharacter* NewPawn = Cast<APaintForgeCharacter>(Ctrl->GetPawn()))
		{
			if (UPFHealthComponent* Health = NewPawn->GetHealth())
			{
				Health->ResetForRound(RoundHP);
			}
		}
	}
}

void APaintForgeGameMode::RespawnVictimAtTeamSpawn(APaintForgeCharacter* Victim)
{
	if (!Victim)
	{
		return;
	}
	// Timed reset-in-place (Skirmish + the Respawn variant): the victim is NOT marked dead, move-locked,
	// or death-cammed — after RespawnDelay it heals to full and teleports to its team spawn. PS is
	// re-fetched inside the timer via the weak victim (safe if it despawned). Works for players and bots.
	TWeakObjectPtr<APaintForgeCharacter> WeakVictim(Victim);
	TWeakObjectPtr<APaintForgeGameMode> WeakThis(this);
	FTimerHandle RespawnHandle;
	GetWorldTimerManager().SetTimer(RespawnHandle,
		FTimerDelegate::CreateLambda([WeakThis, WeakVictim]()
		{
			if (WeakThis.IsValid() && WeakVictim.IsValid())
			{
				WeakVictim->GetHealth()->ResetForRound(3);
				if (APaintForgePlayerState* PS = WeakVictim->GetPlayerState<APaintForgePlayerState>())
				{
					WeakThis->TeleportPawnTo(WeakVictim.Get(), WeakThis->GetSpawnTransform(PS));
				}
			}
		}),
		RespawnDelay, false);
}

// ---------------------------------------------------------------------------
// Bots (fill teams to the selected format — server only)
// ---------------------------------------------------------------------------

int32 APaintForgeGameMode::GetTeamCountByKind(uint8 Team, bool bBotsOnly) const
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return 0;
	}
	int32 Count = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (PS && PS->TeamId == Team && (!bBotsOnly || PS->IsABot()))
		{
			++Count;
		}
	}
	return Count;
}

APaintForgePlayerState* APaintForgeGameMode::AddBot(uint8 Team)
{
	UWorld* World = GetWorld();
	if (!World || Team > 1)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// bWantsPlayerState=true (ctor) => the controller's PostInitializeComponents already created and
	// registered an APaintForgePlayerState in GameState->PlayerArray during SpawnActor.
	APFBotController* Bot = World->SpawnActor<APFBotController>(APFBotController::StaticClass(),
		FTransform::Identity, Params);
	APaintForgePlayerState* PS = Bot ? Bot->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (!PS)
	{
		if (Bot) { Bot->Destroy(); }
		UE_LOG(PaintForgeLog, Warning, TEXT("GameMode: AddBot failed (no PlayerState)"));
		return nullptr;
	}

	PS->SetIsABot(true);
	PS->ServerSetTeam(Team, FindFreeRosterIndex());   // same team/roster path as PostLogin
	PS->SetPlayerName(FString::Printf(TEXT("Bot %d"), PS->RosterIndex + 1));
	PS->bAliveInRound = true;
	// The pawn itself is spawned by the caller's reset loop (RespawnCombatant → RestartPlayer), exactly
	// like a human — that path also binds Health->OnEliminatedEvent so bot deaths reach the match logic.
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: added bot '%s' (team %d, roster %d)"),
		*PS->GetPlayerName(), Team, PS->RosterIndex);
	return PS;
}

void APaintForgeGameMode::FillBotsToFormat()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!bFillWithBots || !GS)
	{
		return;
	}
	const int32 Target = FMath::Clamp<int32>(GS->TargetTeamSize, 1, PFGrid::SpawnPointsPerTeam);
	for (uint8 Team = 0; Team <= 1; ++Team)
	{
		int32 Have = GetTeamCountByKind(Team, /*bBotsOnly=*/false);
		int32 Guard = 0;
		while (Have < Target && Guard < PFGrid::MaxRosterSlots)
		{
			if (!AddBot(Team))
			{
				break;   // roster full or spawn failure
			}
			++Have;
			++Guard;
		}
	}
}

void APaintForgeGameMode::TrimOneBotFromTeam(uint8 Team)
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (PS && PS->IsABot() && PS->TeamId == Team)
		{
			if (APFBotController* Bot = Cast<APFBotController>(PS->GetOwningController()))
			{
				if (APawn* Pawn = Bot->GetPawn())
				{
					Bot->UnPossess();
					Pawn->Destroy();
				}
				Bot->Destroy();   // AController::Destroyed unregisters the PlayerState
			}
			return;   // one is enough
		}
	}
}

void APaintForgeGameMode::RemoveAllBots()
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	// Collect first — destroying a controller mutates PlayerArray under the iterator.
	TArray<APFBotController*> Bots;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (PS && PS->IsABot())
		{
			if (APFBotController* Bot = Cast<APFBotController>(PS->GetOwningController()))
			{
				Bots.Add(Bot);
			}
		}
	}
	for (APFBotController* Bot : Bots)
	{
		if (!Bot)
		{
			continue;
		}
		if (APawn* Pawn = Bot->GetPawn())
		{
			Bot->UnPossess();
			Pawn->Destroy();
		}
		Bot->Destroy();
	}
}

void APaintForgeGameMode::BeginLiveRound()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;
	}

	if (GS->MatchType == EPFMatchType::Skirmish)
	{
		// One continuous Live period; the match ends by tag-cap (NotifyPawnEliminated) or the timer.
		// NO CheckElimVictory — Skirmish never resolves by team-wipe (everyone respawns).
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&APaintForgeGameMode::ResolveSkirmishOnTimer, SkirmishMatchDuration, false);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Skirmish LIVE (%.0f s, first to %d tags)"),
			SkirmishMatchDuration, SkirmishTagTarget);
		return;
	}

	const float Duration = bSuddenDeathRoundActive ? SuddenDeathDuration : EffectiveRoundDuration;
	GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + Duration);
	ApplyServerMoveLocks();
	GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
		&APaintForgeGameMode::ResolveRoundOnTimer, Duration, false);
	// Breakout horn: clients play UPFCombatAudio::PlayBreakout from the RoundState delegate (T6).

	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: round %d LIVE (%.0f s)"), GS->RoundNumber, Duration);

	// A team can already be empty here (whole team disconnected during Freeze/Intermission).
	// CheckElimVictory handles 0-vs-N and 0-vs-0, so the round resolves instead of running 90 s.
	CheckElimVictory();
}

void APaintForgeGameMode::ResolveRoundOnTimer()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	// Timer expiry: more players alive wins; equal alive = draw round (no point).
	uint8 Winner = TeamNone;
	if (GS->AliveCounts[0] > GS->AliveCounts[1])
	{
		Winner = 0;
	}
	else if (GS->AliveCounts[1] > GS->AliveCounts[0])
	{
		Winner = 1;
	}
	EndRound(Winner);
}

void APaintForgeGameMode::ResolveSkirmishOnTimer()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	// Timer expiry: more tags wins; equal = draw (overtime is a future extension).
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1])
	{
		Winner = 0;
	}
	else if (GS->TeamScores[1] > GS->TeamScores[0])
	{
		Winner = 1;
	}
	EndSkirmish(Winner);
}

void APaintForgeGameMode::EndSkirmish(uint8 WinnerTeam)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;   // guards the tag-cap + timer double-fire race
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	PendingMatchWinner = WinnerTeam;
	PendingMatchResult = MakeMatchResult(WinnerTeam);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Skirmish over — winner team %d (%d-%d)"),
		WinnerTeam, GS->TeamScores[0], GS->TeamScores[1]);
	SetPhase(EPFMatchPhase::Vote);   // same Combat→Vote jump EndRound uses for a decided match
}

void APaintForgeGameMode::CheckSkirmishAbandon()
{
	// Disconnect path: Skirmish never resolves by team-wipe (tagged players respawn), but a whole team
	// LEAVING has no respawn — end promptly instead of running the full clock with no opponents. In
	// Skirmish, bAliveInRound is never cleared by a tag, so AliveCounts == connected teamed players.
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Skirmish
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	const uint8 A = GS->AliveCounts[0];
	const uint8 B = GS->AliveCounts[1];
	if (A > 0 && B > 0)
	{
		return;   // both sides still present
	}
	uint8 Winner = TeamNone;   // both empty → draw
	if (A == 0 && B > 0) { Winner = 1; }
	else if (B == 0 && A > 0) { Winner = 0; }
	EndSkirmish(Winner);   // clears the round timer + jumps Combat→Vote (double-fire guarded)
}

void APaintForgeGameMode::CheckElimVictory()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	if (RespawnMode == EPFRespawnMode::Respawn || GS->MatchType == EPFMatchType::Skirmish)
	{
		return;   // respawn variant / Skirmish: resolve on the timer / tag-cap only, never by team-wipe
	}

	const uint8 AliveA = GS->AliveCounts[0];
	const uint8 AliveB = GS->AliveCounts[1];
	if (AliveA > 0 && AliveB > 0)
	{
		return;
	}
	uint8 Winner = TeamNone;   // both empty (mutual leave) = draw
	if (AliveA == 0 && AliveB > 0)
	{
		Winner = 1;
	}
	else if (AliveB == 0 && AliveA > 0)
	{
		Winner = 0;
	}
	EndRound(Winner);
}

void APaintForgeGameMode::EndRound(uint8 WinnerTeam)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);

	// T18 scoring: survival 25 to everyone still alive, round win 50 to every winning teammate.
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		if (PS->bAliveInRound)
		{
			PS->ServerAddScore(ScoreRoundSurvive);
		}
		if (WinnerTeam <= 1 && PS->TeamId == WinnerTeam)
		{
			PS->ServerAddScore(ScoreRoundWin);
		}
	}

	uint8 WinsA = GS->TeamRoundWins[0];
	uint8 WinsB = GS->TeamRoundWins[1];
	if (WinnerTeam == 0)
	{
		++WinsA;
	}
	else if (WinnerTeam == 1)
	{
		++WinsB;
	}
	GS->ServerSetTeamRoundWins(WinsA, WinsB);

	// Decide what follows the intermission.
	bPendingMatchOver = false;
	PendingMatchWinner = TeamNone;

	if (bSuddenDeathRoundActive)
	{
		// Sudden death always ends the match; timer tie = match draw.
		bPendingMatchOver = true;
		PendingMatchWinner = WinnerTeam;
	}
	else if (WinsA >= EffectiveRoundWinsToTake || WinsB >= EffectiveRoundWinsToTake)
	{
		bPendingMatchOver = true;
		PendingMatchWinner = (WinsA >= EffectiveRoundWinsToTake) ? 0 : 1;
	}
	else if (GS->RoundNumber >= EffectiveMaxRounds)
	{
		if (WinsA != WinsB)
		{
			bPendingMatchOver = true;
			PendingMatchWinner = (WinsA > WinsB) ? 0 : 1;
		}
		else
		{
			bPendingSuddenDeath = true;   // post-max deadlock → one sudden-death round
		}
	}

	if (bPendingMatchOver)
	{
		PendingMatchResult = MakeMatchResult(PendingMatchWinner);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: match decided (winner %d, %d-%d)"),
			PendingMatchWinner, WinsA, WinsB);
		SetPhase(EPFMatchPhase::Vote);
		return;
	}

	GS->ServerSetRoundState(EPFRoundState::Intermission, GS->GetServerWorldTimeSeconds() + IntermissionDuration);
	ApplyServerMoveLocks();
	GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
		&APaintForgeGameMode::OnIntermissionEnd, IntermissionDuration, false);

	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: round %d over (winner %d) - score %d-%d"),
		GS->RoundNumber, WinnerTeam, WinsA, WinsB);
}

void APaintForgeGameMode::OnIntermissionEnd()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;
	}
	if (bPendingSuddenDeath)
	{
		bPendingSuddenDeath = false;
		StartSuddenDeath();
	}
	else
	{
		StartNextRound();
	}
}

void APaintForgeGameMode::StartSuddenDeath()
{
	// The one entry point into the sudden-death round (reached from OnIntermissionEnd after a
	// post-max-rounds deadlock flagged bPendingSuddenDeath in EndRound).
	bPendingSuddenDeath = true;
	StartNextRound();
}

FPFMatchResult APaintForgeGameMode::MakeMatchResult(uint8 MatchWinner) const
{
	const APaintForgeGameState* GS = GetPFGameState();
	FPFMatchResult Result;
	Result.WinnerTeam = MatchWinner;
	if (GS)
	{
		if (GS->MatchType == EPFMatchType::Skirmish)
		{
			// Reuse the round-win fields for the final tag counts (semantic reuse; clamp to field width).
			Result.RoundWinsA = static_cast<uint8>(FMath::Min<int32>(GS->TeamScores[0], 255));
			Result.RoundWinsB = static_cast<uint8>(FMath::Min<int32>(GS->TeamScores[1], 255));
			Result.RoundsPlayed = 1;
		}
		else
		{
			Result.RoundWinsA = GS->TeamRoundWins[0];
			Result.RoundWinsB = GS->TeamRoundWins[1];
			Result.RoundsPlayed = GS->RoundNumber;
		}
		Result.MatchDurationSec = FMath::RoundToInt32(
			FMath::Max(0.f, GS->GetServerWorldTimeSeconds() - MatchStartServerTime));
	}
	Result.bSuddenDeath = bSuddenDeathPlayed;   // always false in Skirmish
	return Result;
}

// ---------------------------------------------------------------------------
// Eliminations
// ---------------------------------------------------------------------------

void APaintForgeGameMode::NotifyPawnEliminated(APaintForgeCharacter* Victim, const FPFPaintHitInfo& FinalHit)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || !Victim)
	{
		return;
	}
	APaintForgePlayerState* VictimPS = Victim->GetPlayerState<APaintForgePlayerState>();
	if (!VictimPS)
	{
		return;
	}
	APaintForgePlayerState* ShooterPS = FinalHit.ShooterPS;

	// CONTRACT-GAP: the contract specifies self-resetting dummies (T29) but is silent on player
	// pawns eliminated in the warm-up pen (Lobby fire is live, T21). Smallest implementation:
	// stats-free 1 s reset back to the pen, mirroring the dummy behavior.
	if (GS->Phase == EPFMatchPhase::Lobby)
	{
		TWeakObjectPtr<APaintForgeCharacter> WeakVictim(Victim);
		TWeakObjectPtr<APaintForgeGameMode> WeakThis(this);
		FTimerHandle ResetHandle;
		GetWorldTimerManager().SetTimer(ResetHandle,
			FTimerDelegate::CreateLambda([WeakThis, WeakVictim]()
			{
				if (WeakThis.IsValid() && WeakVictim.IsValid())
				{
					WeakVictim->GetHealth()->ResetForRound(3);
					if (APaintForgePlayerState* PS = WeakVictim->GetPlayerState<APaintForgePlayerState>())
					{
						WeakThis->TeleportPawnTo(WeakVictim.Get(), WeakThis->GetSpawnTransform(PS));
					}
				}
			}),
			1.f, false);
		return;
	}

	if (GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("GameMode: elimination outside a live round ignored (%s)"),
			*VictimPS->GetPlayerName());
		return;
	}

	// Bookkeeping + feed (elim feed is GameState-array replicated — late-join correct, 02 D11).
	VictimPS->TimesEliminated = VictimPS->TimesEliminated + 1;
	if (ShooterPS && FinalHit.ShooterTeam != VictimPS->TeamId)
	{
		ShooterPS->Eliminations = ShooterPS->Eliminations + 1;
		ShooterPS->ServerAddScore(ScoreElimination);
	}

	FPFElimEntry Entry;
	Entry.ShooterName = ShooterPS ? ShooterPS->GetPlayerName() : FString();
	Entry.ShooterTeam = FinalHit.ShooterTeam;
	Entry.VictimName = VictimPS->GetPlayerName();
	Entry.VictimTeam = VictimPS->TeamId;
	Entry.ServerTime = GS->GetServerWorldTimeSeconds();
	GS->ServerAddElimEntry(Entry);

	// SKIRMISH: credit the shooter's TEAM a tag, check the tag cap, then respawn the victim in place.
	// Inserted BEFORE the RoundElimination path and returns before it — the victim is never marked
	// "out for the round", move-locked, death-cammed, or removed from AliveCounts.
	if (GS->MatchType == EPFMatchType::Skirmish)
	{
		if (ShooterPS && FinalHit.ShooterTeam != VictimPS->TeamId && FinalHit.ShooterTeam <= 1)
		{
			uint16 TagsA = GS->TeamScores[0];
			uint16 TagsB = GS->TeamScores[1];
			if (FinalHit.ShooterTeam == 0) { ++TagsA; } else { ++TagsB; }
			GS->ServerSetTeamScores(TagsA, TagsB);   // replicates + HUD refresh via OnRep_Score
			if ((FinalHit.ShooterTeam == 0 ? TagsA : TagsB) >= SkirmishTagTarget)
			{
				EndSkirmish(FinalHit.ShooterTeam);
				return;
			}
		}
		RespawnVictimAtTeamSpawn(Victim);
		return;
	}

	if (RespawnMode == EPFRespawnMode::Respawn)
	{
		RespawnVictimAtTeamSpawn(Victim);   // 04 Variant B: timed reset in place of round elimination
		return;
	}

	// Round elimination (B1): out for the round, movement frozen, death cam → teammate
	// spectate (T5). Corpse collision handling (paintball 0.5 s window, capsule off) is
	// UPFHealthComponent's job (§3.4); the move lock here keeps the hidden pawn from being
	// walked around for the rest of the round.
	VictimPS->bAliveInRound = false;
	if (APaintForgePlayerController* VictimPC = Cast<APaintForgePlayerController>(VictimPS->GetPlayerController()))
	{
		VictimPC->SetEliminatedMoveLock(true);
		VictimPC->StartDeathCamera();
	}
	RecountAlive();

	// Already-dead spectators whose view target was the victim's now-hidden pawn move on to
	// another living teammate (or back to their own body when none remain).
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(It->Get()))
		{
			PC->RetargetSpectatorFrom(Victim);
		}
	}

	CheckElimVictory();
}

void APaintForgeGameMode::RecountAlive()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	uint8 Alive[2] = {0, 0};
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (PS && PS->bAliveInRound && PS->TeamId <= 1)
		{
			++Alive[PS->TeamId];
		}
	}
	GS->ServerSetAliveCounts(Alive[0], Alive[1]);
}

// ---------------------------------------------------------------------------
// Voting
// ---------------------------------------------------------------------------

void APaintForgeGameMode::SubmitVote(APaintForgePlayerController* Voter, EPFThumbVote Thumb,
	const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Vote || !Voter)
	{
		return;
	}
	APaintForgePlayerState* PS = Voter->GetPlayerState<APaintForgePlayerState>();
	if (!PS || PS->bHasVoted)
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("GameMode: duplicate/invalid vote rejected"));
		return;
	}

	// Server re-enforces T30: valid IDs 1..8, deduped, no liked∩disliked, ≤4 combined.
	// A tampered out-of-range thumb byte clamps to Abstained so the tally always sums to the
	// player count on the Results screen.
	if (Thumb != EPFThumbVote::Up && Thumb != EPFThumbVote::Down)
	{
		Thumb = EPFThumbVote::Abstained;
	}
	TArray<uint8> Liked = LikedIds;
	TArray<uint8> Disliked = DislikedIds;
	SanitizeVoteIds(Liked, Disliked);

	PS->bHasVoted = true;

	FPFVoteTally Tally = GS->VoteTally;
	switch (Thumb)
	{
	case EPFThumbVote::Up:        ++Tally.Up; break;
	case EPFThumbVote::Down:      ++Tally.Down; break;
	case EPFThumbVote::Abstained: ++Tally.Abstained; break;
	}
	for (uint8 Id : Liked)
	{
		++Tally.LikedCounts[Id - 1];
	}
	for (uint8 Id : Disliked)
	{
		++Tally.DislikedCounts[Id - 1];
	}
	GS->ServerSetVoteTally(Tally);

	if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
	{
		FPFVoteRecord Record;
		Record.VoterGuidHash = PS->PlayerGuidHash;
		Record.VoterTeam = PS->TeamId;
		Record.Thumb = Thumb;
		Record.LikedIds = Liked;
		Record.DislikedIds = Disliked;
		Rating->AddVote(Record, /*bBuiltHalfA=*/PS->TeamId == 0);   // each team builds its own half
	}

	// T3: vote auto-advances early once every connected player has voted.
	CheckAllVotesIn(nullptr);
}

void APaintForgeGameMode::CheckAllVotesIn(const APaintForgePlayerState* IgnorePS)
{
	// T3 early-advance, re-evaluated on every vote AND on disconnect (the last non-voter
	// leaving completes the condition; their lingering PlayerState is excluded). No abstain
	// fill needed here — everyone counted has voted, so FinalizeVotePhase has nothing to add.
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Vote)
	{
		return;
	}
	for (APlayerState* OtherBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* Other = Cast<APaintForgePlayerState>(OtherBase);
		if (Other && Other != IgnorePS && !Other->IsABot() && !Other->bHasVoted)
		{
			return;   // bots don't vote — they don't hold the vote phase open
		}
	}
	SetPhase(EPFMatchPhase::Results);
}

void APaintForgeGameMode::FinalizeVotePhase()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Vote)
	{
		return;
	}

	// Timeout: everyone who never submitted records as abstained (01 §4.2) so the match JSON
	// still carries one vote entry per connected player.
	FPFVoteTally Tally = GS->VoteTally;
	UPFRatingSubsystem* Rating = GetRatingSubsystem();
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS || PS->bHasVoted || PS->IsABot())
		{
			continue;   // bots don't vote — excluding them here matches CheckAllVotesIn so the
			            // timeout tally + persisted rating record aren't polluted with bot abstains
		}
		PS->bHasVoted = true;
		++Tally.Abstained;
		if (Rating)
		{
			FPFVoteRecord Record;
			Record.VoterGuidHash = PS->PlayerGuidHash;
			Record.VoterTeam = PS->TeamId;
			Record.Thumb = EPFThumbVote::Abstained;
			Rating->AddVote(Record, /*bBuiltHalfA=*/PS->TeamId == 0);
		}
	}
	GS->ServerSetVoteTally(Tally);

	SetPhase(EPFMatchPhase::Results);
}

void APaintForgeGameMode::SanitizeVoteIds(TArray<uint8>& InOutLiked, TArray<uint8>& InOutDisliked)
{
	auto CleanList = [](TArray<uint8>& List)
	{
		TArray<uint8> Clean;
		for (uint8 Id : List)
		{
			if (Id >= 1 && Id <= 8 && !Clean.Contains(Id))
			{
				Clean.Add(Id);
			}
		}
		List = MoveTemp(Clean);
	};
	CleanList(InOutLiked);
	CleanList(InOutDisliked);

	// A category cannot be both liked and disliked (chip cycles through states, 01 §4.2).
	InOutDisliked.RemoveAll([&InOutLiked](uint8 Id) { return InOutLiked.Contains(Id); });

	// ≤4 combined selections (T30); liked entries take precedence on overflow.
	while (InOutLiked.Num() + InOutDisliked.Num() > 4)
	{
		if (InOutDisliked.Num() > 0)
		{
			InOutDisliked.Pop();
		}
		else
		{
			InOutLiked.Pop();
		}
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void APaintForgeGameMode::ApplyServerMoveLocks()
{
	// §4.5: move input ignored in Freeze / Intermission / Vote / Results; look stays free.
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	const bool bLocked =
		(GS->Phase == EPFMatchPhase::Combat && (GS->RoundState == EPFRoundState::Freeze ||
		                                        GS->RoundState == EPFRoundState::Intermission))
		|| GS->Phase == EPFMatchPhase::Vote
		|| GS->Phase == EPFMatchPhase::Results;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(It->Get()))
		{
			PC->ApplyServerMoveLock(bLocked);
		}
	}
}

void APaintForgeGameMode::ResetPlayerMatchStats()
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
		{
			PS->Eliminations = 0;
			PS->TimesEliminated = 0;
			PS->MatchScore = 0;
			PS->bHasVoted = false;
			PS->ForceNetUpdate();
		}
	}
}

APaintForgeGameState* APaintForgeGameMode::GetPFGameState() const
{
	return GetGameState<APaintForgeGameState>();
}

UPFRatingSubsystem* APaintForgeGameMode::GetRatingSubsystem() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UPFRatingSubsystem>() : nullptr;
}

void APaintForgeGameMode::GetTeamCounts(int32& OutTeamA, int32& OutTeamB) const
{
	OutTeamA = 0;
	OutTeamB = 0;
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		if (PS->TeamId == 0)
		{
			++OutTeamA;
		}
		else if (PS->TeamId == 1)
		{
			++OutTeamB;
		}
	}
}

bool APaintForgeGameMode::AreAllPlayersReady(const APaintForgePlayerState* IgnorePS) const
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return false;
	}
	int32 Counted = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS || PS == IgnorePS || PS->IsABot())
		{
			continue;   // bots never ready up — they don't gate the human ready check
		}
		if (!PS->bReady)
		{
			return false;
		}
		++Counted;
	}
	return Counted > 0;
}

uint8 APaintForgeGameMode::FindFreeRosterIndex() const
{
	const APaintForgeGameState* GS = GetPFGameState();
	for (uint8 Candidate = 0; Candidate < PFGrid::MaxRosterSlots; ++Candidate)
	{
		bool bTaken = false;
		if (GS)
		{
			for (APlayerState* PSBase : GS->PlayerArray)
			{
				const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
				if (PS && PS->RosterIndex == Candidate)
				{
					bTaken = true;
					break;
				}
			}
		}
		if (!bTaken)
		{
			return Candidate;
		}
	}
	UE_LOG(PaintForgeLog, Error, TEXT("GameMode: roster full (12) - reusing slot 11"));
	return PFGrid::MaxRosterSlots - 1;
}

void APaintForgeGameMode::ComputeEffectiveScaling()
{
	// Skirmish: one continuous timed period, win by tag count — bypass all round scaling. RoundWinsToTake
	// carries the tag TARGET for the HUD (clamped to its uint8 field; the real win-check uses the uint16).
	if (APaintForgeGameState* SkGS = GetPFGameState())
	{
		if (SkGS->MatchType == EPFMatchType::Skirmish)
		{
			const uint8 TargetU8 = static_cast<uint8>(FMath::Min<int32>(SkirmishTagTarget, 255));
			EffectiveRoundWinsToTake = TargetU8;
			EffectiveMaxRounds = 1;
			EffectiveRoundDuration = SkirmishMatchDuration;
			SkGS->ServerSetRoundWinsToTake(TargetU8);
			UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Skirmish — first to %d tags, %.0f s"),
				SkirmishTagTarget, SkirmishMatchDuration);
			return;
		}
	}

	// Format = the SELECTED team size (bots fill to it), not the live human count — so a 2-human 4v4
	// still plays first-to-4, not the ≤2v2 small format. With bots off, fall back to live team counts.
	const APaintForgeGameState* GS = GetPFGameState();
	int32 FormatTeamSize = GS ? static_cast<int32>(GS->TargetTeamSize) : static_cast<int32>(DefaultTeamSize);
	if (!bFillWithBots)
	{
		int32 TeamA = 0, TeamB = 0;
		GetTeamCounts(TeamA, TeamB);
		FormatTeamSize = FMath::Max(TeamA, TeamB);
	}

	if (FormatTeamSize <= 2)   // 1v1–2v2 (T15)
	{
		EffectiveRoundWinsToTake = SmallRoundWinsToTake;
		EffectiveMaxRounds = SmallMaxRounds;
		EffectiveRoundDuration = SmallRoundDuration;
	}
	else
	{
		EffectiveRoundWinsToTake = RoundWinsToTakeMatch;
		EffectiveMaxRounds = MaxRounds;
		EffectiveRoundDuration = RoundDuration;
	}
	// Replicate the resolved win threshold so the HUD renders the right pip count instead of inferring
	// it from the live (bot-padded / leaver-shrunk) player count.
	if (APaintForgeGameState* MutableGS = GetPFGameState())
	{
		MutableGS->ServerSetRoundWinsToTake(EffectiveRoundWinsToTake);
	}

	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: match format first-to-%d, max %d rounds, %.0f s rounds"),
		EffectiveRoundWinsToTake, EffectiveMaxRounds, EffectiveRoundDuration);
}
