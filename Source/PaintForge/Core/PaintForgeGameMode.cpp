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
#include "Objectives/PFControlPointActor.h"
#include "Objectives/PFFlagActor.h"
#include "Objectives/PFObjectiveLayout.h"
#include "Voting/PFRatingSubsystem.h"

#include "GameFramework/PawnMovementComponent.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
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

void APaintForgeGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	// Kids drop Wi‑Fi / force-quit mid-match; default UE timeouts leave ghost PlayerStates for a long
	// time and make "two of me" / stuck Ready rows. Snappier drop detection for LAN playtest.
	if (UWorld* World = GetWorld())
	{
		if (UNetDriver* Net = World->GetNetDriver())
		{
			Net->ConnectionTimeout = 12.f;
			Net->InitialConnectTimeout = 20.f;
			UE_LOG(PaintForgeLog, Log, TEXT("GameMode: net timeouts Connection=%.0fs Initial=%.0fs"),
				Net->ConnectionTimeout, Net->InitialConnectTimeout);
		}
	}
}

void APaintForgeGameMode::BeginPlay()
{
	Super::BeginPlay();
	SpawnArenaActors();

	// NetDriver is often created after InitGame for listen hosts — re-apply timeouts here.
	if (UWorld* World = GetWorld())
	{
		if (UNetDriver* Net = World->GetNetDriver())
		{
			Net->ConnectionTimeout = 12.f;
			Net->InitialConnectTimeout = 20.f;
		}
	}

	EffectiveRoundWinsToTake = RoundWinsToTakeMatch;
	EffectiveMaxRounds = MaxRounds;
	EffectiveRoundDuration = RoundDuration;

	if (APaintForgeGameState* GS = GetPFGameState())
	{
		GS->ServerSetTargetTeamSize(DefaultTeamSize);   // 4v4 default; host can switch to 6v6 in Lobby
		GS->ServerSetBuildMode(DefaultBuildMode);       // Creative default; host picks in Lobby / menu
		GS->ServerSetMatchType(DefaultMatchType);       // Skirmish default (kids)
	}

#if !UE_BUILD_SHIPPING
	// Deploy/playtest/smoke-improvement.ps1: editor -game -nullrhi -SmokeImprovement
	if (FParse::Param(FCommandLine::Get(), TEXT("SmokeImprovement")))
	{
		bFillWithBots = true;   // need two sides for a normal lobby→build
		SmokeImprovementStep = 0;
		SmokeImprovementElapsed = 0.f;
		GetWorldTimerManager().SetTimer(SmokeImprovementTimer, this,
			&APaintForgeGameMode::TickSmokeImprovement, 0.5f, true);
		UE_LOG(PaintForgeLog, Warning, TEXT("SMOKE: Improvement path armed (-SmokeImprovement)"));
	}
#endif
}

#if !UE_BUILD_SHIPPING
namespace
{
	/** Write a tiny both-team fort into ProjectSavedDir/Arenas so packaged smokes don't depend on host paths. */
	bool SmokeWriteCommunityArenaSeed()
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Arenas");
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		const FString Path = Dir / TEXT("arena_99999999_smokeseed.json");
		// Newest-wins loader sorts by name — high timestamp prefix stays on top of empty match dumps.
		const TCHAR* Json =
			TEXT("{\n")
			TEXT("  \"schema\": 1,\n")
			TEXT("  \"game\": \"PaintForge\",\n")
			TEXT("  \"matchId\": \"smoke-seed\",\n")
			TEXT("  \"createdUtc\": \"2026-07-12T00:00:00Z\",\n")
			TEXT("  \"teamSize\": 2,\n")
			TEXT("  \"grid\": { \"cellUU\": 400, \"subUU\": 100, \"wallH\": 300, \"cellsX\": 16, \"cellsY\": 10, \"levels\": 4 },\n")
			TEXT("  \"arenaId\": \"smoke\",\n")
			TEXT("  \"halfHashA\": \"smoke\",\n")
			TEXT("  \"halfHashB\": \"smoke\",\n")
			TEXT("  \"pieces\": [\n")
			TEXT("    { \"id\": 1, \"t\": 1, \"x\": 8,  \"y\": 16, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 0 },\n")
			TEXT("    { \"id\": 2, \"t\": 1, \"x\": 12, \"y\": 16, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 0 },\n")
			TEXT("    { \"id\": 3, \"t\": 0, \"x\": 8,  \"y\": 16, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 0 },\n")
			TEXT("    { \"id\": 4, \"t\": 1, \"x\": 40, \"y\": 16, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 1 },\n")
			TEXT("    { \"id\": 5, \"t\": 1, \"x\": 44, \"y\": 16, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 1 },\n")
			TEXT("    { \"id\": 6, \"t\": 0, \"x\": 40, \"y\": 16, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 1 },\n")
			TEXT("    { \"id\": 7, \"t\": 4, \"x\": 10, \"y\": 18, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 0 },\n")
			TEXT("    { \"id\": 8, \"t\": 4, \"x\": 42, \"y\": 18, \"z\": 0, \"r\": 0, \"own\": 0, \"team\": 1 }\n")
			TEXT("  ]\n")
			TEXT("}\n");
		if (!FFileHelper::SaveStringToFile(Json, *Path))
		{
			UE_LOG(PaintForgeLog, Error, TEXT("SMOKE: failed to write arena seed %s"), *Path);
			return false;
		}
		UE_LOG(PaintForgeLog, Warning, TEXT("SMOKE: wrote community seed %s (ProjectSavedDir=%s)"),
			*Path, *FPaths::ProjectSavedDir());
		return true;
	}
}

void APaintForgeGameMode::TickSmokeImprovement()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	SmokeImprovementElapsed += 0.5f;

	// Hard timeout — never hang a CI/playtest host.
	if (SmokeImprovementElapsed > 90.f)
	{
		UE_LOG(PaintForgeLog, Error, TEXT("SMOKE: Improvement FAIL (timeout %.0fs, phase=%d, basePieces=%d)"),
			SmokeImprovementElapsed, static_cast<int32>(GS->Phase), GS->CommunityBasePieces);
		GetWorldTimerManager().ClearTimer(SmokeImprovementTimer);
		FGenericPlatformMisc::RequestExit(false);
		return;
	}

	switch (SmokeImprovementStep)
	{
	case 0:   // Wait for a human host to exist in Lobby, then configure Improvement.
		if (GS->Phase != EPFMatchPhase::Lobby)
		{
			return;
		}
		{
			bool bHaveHost = false;
			for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
			{
				if (APaintForgePlayerController* PC = Cast<APaintForgePlayerController>(It->Get()))
				{
					if (PC->IsLocalController())
					{
						bHaveHost = true;
						break;
					}
				}
			}
			if (!bHaveHost)
			{
				return;
			}
		}
		// Always re-seed under the runtime ProjectSavedDir (editor vs packaged differ).
		SmokeWriteCommunityArenaSeed();
		HostSetMatchType(EPFMatchType::Skirmish);       // team mode so bots fill
		HostSetBuildMode(EPFBuildMode::Improvement);
		HostSetFormat(2);                               // small format → faster fill
		UE_LOG(PaintForgeLog, Warning, TEXT("SMOKE: configured Improvement + Skirmish 2v2"));
		SmokeImprovementStep = 1;
		break;

	case 1:   // Force the lobby countdown once (host path).
		if (GS->Phase != EPFMatchPhase::Lobby)
		{
			SmokeImprovementStep = 2;
			return;
		}
		if (!bLobbyCountdownActive)
		{
			HostForceStart();
			UE_LOG(PaintForgeLog, Warning, TEXT("SMOKE: HostForceStart (lobby countdown)"));
		}
		if (GS->Phase == EPFMatchPhase::Build || GS->Phase == EPFMatchPhase::Combat)
		{
			SmokeImprovementStep = 2;
		}
		break;

	case 2:   // Build inject is synchronous at Lobby→Build — assert CommunityBasePieces.
		if (GS->Phase == EPFMatchPhase::Lobby)
		{
			return;   // still counting down
		}
		if (GS->Phase == EPFMatchPhase::Build || GS->Phase == EPFMatchPhase::Combat)
		{
			const uint16 N = GS->CommunityBasePieces;
			if (N > 0)
			{
				UE_LOG(PaintForgeLog, Warning,
					TEXT("SMOKE: Improvement PASS (CommunityBasePieces=%d, phase=%d)"),
					N, static_cast<int32>(GS->Phase));
			}
			else
			{
				UE_LOG(PaintForgeLog, Error,
					TEXT("SMOKE: Improvement FAIL (CommunityBasePieces=0 — seed Saved/Arenas or inject broke)"));
			}
			SmokeImprovementStep = 3;
			GetWorldTimerManager().ClearTimer(SmokeImprovementTimer);
			// Short grace so the log flushes, then exit.
			FTimerHandle ExitHandle;
			GetWorldTimerManager().SetTimer(ExitHandle, FTimerDelegate::CreateLambda([]()
			{
				FGenericPlatformMisc::RequestExit(false);
			}), 1.5f, false);
		}
		break;

	default:
		break;
	}
}
#endif

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
	// Drop any human PlayerStates that lost their controller (ghosts from crash/force-quit).
	// Must run before team assignment so roster slots free up for the rejoiner.
	ScrubGhostPlayerStates(/*KeepPS=*/nullptr);

	// Assign team + roster slot BEFORE Super::PostLogin so the initial pawn spawn already
	// knows its side (RestartPlayer → GetSpawnTransform reads TeamId).
	APaintForgePlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<APaintForgePlayerState>() : nullptr;
	if (PS && PS->TeamId == TeamNone)
	{
		const APaintForgeGameState* PreGS = GetPFGameState();
		if (PreGS && PreGS->MatchType == EPFMatchType::FreeForAll)
		{
			// Unique combat id (= roster) so B12 never treats two FFA players as teammates.
			const uint8 Roster = FindFreeRosterIndex();
			PS->ServerSetTeam(Roster, Roster);
		}
		else
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
		// FreeForAll has no teams — skip the team-size clamp.
		if (bFillWithBots && GetPFGameState() && GetPFGameState()->MatchType != EPFMatchType::FreeForAll
			&& PS->TeamId <= 1 &&
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
		ExitingPS->ServerSetReady(false);   // ghosts must not block lobby Ready
		// CTF: return any carried flag so the match doesn't soft-lock with a phantom carrier.
		if (ExitingPS->bCarryingFlag)
		{
			if (APFFlagActor* Carried = GetFlagForTeam(ExitingPS->CarriedFlagTeam))
			{
				Carried->ServerReturnHome();
			}
			ExitingPS->ServerSetFlagCarry(false, 255);
		}
		ExitingPS->ServerSetStandingOnPoint(255);
		// Free the roster slot so a rejoin doesn't collide (FindFreeRosterIndex scans PlayerArray).
		ExitingPS->ServerSetTeam(TeamNone, 255);
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

	// Hard-destroy lingering human PlayerStates so the lobby never shows a second "ghost" row
	// that can't ready (kids force-quit / Wi‑Fi drop mid-match).
	if (IsValid(ExitingPS) && !ExitingPS->IsABot())
	{
		ExitingPS->Destroy();
	}
	ScrubGhostPlayerStates();

	// A leaver can complete an elimination victory, an all-ready condition, or an all-voted
	// condition. Their PlayerState may still sit in PlayerArray here, so the ready/vote scans
	// take it as an explicit exclusion.
	RecountAlive();
	CheckElimVictory();
	CheckSkirmishAbandon();   // Skirmish: a whole team leaving ends the match (CheckElimVictory no-ops here)
	CheckFreeForAllAbandon(); // FFA: last combatant standing wins
	CheckCaptureFlagAbandon();
	CheckDominationAbandon();
	CheckHardpointAbandon();
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
		// Palette is A/B only; FFA unique combat ids map via % 2 (same as PlayerState ApplyTeamColor).
		if (PS->TeamId != TeamNone)
		{
			Pawn->SetTeamColor(static_cast<uint8>(PS->TeamId % 2));
		}

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

	// FreeForAll: free spawns across both team spawn columns (no team half ownership).
	if (GS && GS->MatchType == EPFMatchType::FreeForAll)
	{
		if (Phase == EPFMatchPhase::Lobby)
		{
			return ArenaShell->GetWarmupSpawnTransform(
				FMath::Min<int32>(PS->RosterIndex, PFGrid::MaxRosterSlots - 1));
		}
		const int32 FreeIdx = static_cast<int32>(PS->RosterIndex) % (PFGrid::SpawnPointsPerTeam * 2);
		const uint8 Side = static_cast<uint8>(FreeIdx / PFGrid::SpawnPointsPerTeam);
		const int32 Slot = FreeIdx % PFGrid::SpawnPointsPerTeam;
		return ArenaShell->GetTeamSpawnTransform(Side, Slot);
	}

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

		// FreeForAll: stamp unique combat TeamIds (= roster) on everyone so B12 never blocks tags.
		if (GS->MatchType == EPFMatchType::FreeForAll)
		{
			for (APlayerState* PSBase : GS->PlayerArray)
			{
				if (APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
				{
					if (PS->RosterIndex != 255)
					{
						PS->ServerSetTeam(PS->RosterIndex, PS->RosterIndex);
					}
				}
			}
		}

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

		// Community inject (after bots exist, grid cleared, before FreezeBuild) so it replicates
		// through the build window and is captured by BeginMatchRecord at Combat. Skipped in Play-only.
		//   Improvement → whole saved arena; both teams improve their half.
		//   Creative    → only an all-bot team's half is filled (nobody is there to build it).
		if (BuildGrid && GS->BuildMode != EPFBuildMode::PlayOnly)
		{
			if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
			{
				if (GS->BuildMode == EPFBuildMode::Improvement)
				{
					TArray<FPFBuildPieceRec> Whole;
					if (Rating->PickCommunityArena(Whole))
					{
						BuildGrid->ServerInjectPieces(Whole);
						UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Improvement loaded %d community pieces"),
							Whole.Num());
					}
					else
					{
						// No Saved/Arenas/*.json with geometry yet — fall back to empty field (Creative-like).
						UE_LOG(PaintForgeLog, Warning,
							TEXT("GameMode: Improvement — no community arena on disk; starting empty"));
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
			// Actual injected count after validation skips (invalid team/type filtered in inject).
			const int32 N = BuildGrid->GetPieces().Num();
			GS->ServerSetCommunityBasePieces(static_cast<uint16>(FMath::Clamp(N, 0, 65535)));
		}
		else
		{
			GS->ServerSetCommunityBasePieces(0);
		}

		// Play-only mode runs all the Build-phase SETUP above (match reset, bots, spawns) but skips the
		// build TIME — flash straight to Combat. (Creative/Improvement get the full build window.)
		// FreeForAll is play-only by nature (solo tags, no build phase).
		const bool bPlayOnly = (GS->BuildMode == EPFBuildMode::PlayOnly)
			|| (GS->MatchType == EPFMatchType::FreeForAll);
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
		// Objective actors live only during combat Live — tear them down on the way out.
		DestroyObjectiveActors();
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
	// FreeForAll has no teams (unique combat ids) — team cycle is a no-op.
	if (GS->MatchType == EPFMatchType::FreeForAll)
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
	// FreeForAll is play-only by design — Creative/Improvement would flash past or confuse the lobby.
	if (GS->MatchType == EPFMatchType::FreeForAll && NewMode != EPFBuildMode::PlayOnly)
	{
		NewMode = EPFBuildMode::PlayOnly;
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: FreeForAll forces PlayOnly build mode"));
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
	// FreeForAll is play-only by design — force the build mode so Lobby UI + Lobby→Build agree.
	if (NewType == EPFMatchType::FreeForAll)
	{
		GS->ServerSetBuildMode(EPFBuildMode::PlayOnly);
	}
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
		GS->RoundNumber, bSuddenDeathRoundActive ? TEXT("SHOWDOWN") : TEXT("normal"));
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
	if (!World)
	{
		return nullptr;
	}
	// Team modes: Team must be 0/1. FreeForAll ignores Team and assigns a unique combat id.
	const APaintForgeGameState* GS = GetPFGameState();
	const bool bFFA = GS && GS->MatchType == EPFMatchType::FreeForAll;
	if (!bFFA && Team > 1)
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
	const uint8 Roster = FindFreeRosterIndex();
	// FFA: unique combat TeamId (= roster) so projectile B12 never treats two players as teammates.
	PS->ServerSetTeam(bFFA ? Roster : Team, Roster);
	PS->SetPlayerName(FString::Printf(TEXT("Bot %d"), PS->RosterIndex + 1));
	PS->bAliveInRound = true;
	// The pawn itself is spawned by the caller's reset loop (RespawnCombatant → RestartPlayer), exactly
	// like a human — that path also binds Health->OnEliminatedEvent so bot deaths reach the match logic.
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: added bot '%s' (team %d, roster %d)"),
		*PS->GetPlayerName(), PS->TeamId, PS->RosterIndex);
	return PS;
}

void APaintForgeGameMode::FillBotsToFormat()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!bFillWithBots || !GS)
	{
		return;
	}

	// FreeForAll: fill total combatants to 2× format size as individuals (not per-team).
	if (GS->MatchType == EPFMatchType::FreeForAll)
	{
		const int32 TargetTotal = FMath::Clamp<int32>(
			static_cast<int32>(GS->TargetTeamSize) * 2, 2, PFGrid::MaxRosterSlots);
		int32 Have = 0;
		for (APlayerState* PSBase : GS->PlayerArray)
		{
			if (Cast<APaintForgePlayerState>(PSBase))
			{
				++Have;
			}
		}
		int32 Guard = 0;
		while (Have < TargetTotal && Guard < PFGrid::MaxRosterSlots)
		{
			if (!AddBot(0))   // Team arg ignored for FFA (unique combat id assigned inside)
			{
				break;
			}
			++Have;
			++Guard;
		}
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

	if (GS->MatchType == EPFMatchType::FreeForAll)
	{
		// Solo continuous Live period — same clock as Skirmish, per-player TagCount win.
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&APaintForgeGameMode::ResolveFreeForAllOnTimer, SkirmishMatchDuration, false);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: FreeForAll LIVE (%.0f s, first to %d tags)"),
			SkirmishMatchDuration, SkirmishTagTarget);
		return;
	}

	if (GS->MatchType == EPFMatchType::CaptureFlag)
	{
		SpawnObjectiveActors();
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&APaintForgeGameMode::ResolveCaptureFlagOnTimer, SkirmishMatchDuration, false);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: CaptureFlag LIVE (%.0f s, first to %d captures)"),
			SkirmishMatchDuration, CaptureFlagTarget);
		return;
	}

	if (GS->MatchType == EPFMatchType::Domination)
	{
		SpawnObjectiveActors();
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&APaintForgeGameMode::ResolveDominationOnTimer, SkirmishMatchDuration, false);
		GetWorldTimerManager().SetTimer(ObjectiveScoreTimerHandle, this,
			&APaintForgeGameMode::TickDominationScoring, ObjectiveScoreInterval, true);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Domination LIVE (%.0f s, first to %d)"),
			SkirmishMatchDuration, SkirmishTagTarget);
		return;
	}

	if (GS->MatchType == EPFMatchType::Hardpoint)
	{
		HardpointActiveSlot = 0;
		SpawnObjectiveActors();
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&APaintForgeGameMode::ResolveHardpointOnTimer, SkirmishMatchDuration, false);
		GetWorldTimerManager().SetTimer(ObjectiveScoreTimerHandle, this,
			&APaintForgeGameMode::TickHardpointScoring, ObjectiveScoreInterval, true);
		GetWorldTimerManager().SetTimer(HardpointRotateTimerHandle, this,
			&APaintForgeGameMode::RotateHardpoint, HardpointRotateInterval, true);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Hardpoint LIVE (%.0f s, first to %d, rotate %.0f s)"),
			SkirmishMatchDuration, SkirmishTagTarget, HardpointRotateInterval);
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

void APaintForgeGameMode::ResolveFreeForAllOnTimer()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	// Most tags wins; exact tie → draw.
	uint16 BestTags = 0;
	int32 BestCount = 0;
	uint8 BestRoster = TeamNone;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS || PS->RosterIndex == 255)
		{
			continue;
		}
		if (PS->TagCount > BestTags)
		{
			BestTags = PS->TagCount;
			BestCount = 1;
			BestRoster = PS->RosterIndex;
		}
		else if (PS->TagCount == BestTags && BestTags > 0)
		{
			++BestCount;
		}
	}
	const uint8 Winner = (BestCount == 1 && BestTags > 0) ? BestRoster : TeamNone;
	EndFreeForAll(Winner);
}

void APaintForgeGameMode::EndFreeForAll(uint8 WinnerRosterOrNone)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;   // tag-cap + timer double-fire guard
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	PendingMatchWinner = WinnerRosterOrNone;   // roster index, or 255 draw (not a team id)
	PendingMatchResult = MakeMatchResult(WinnerRosterOrNone);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: FreeForAll over — winner roster %d"),
		WinnerRosterOrNone);
	SetPhase(EPFMatchPhase::Vote);
}

void APaintForgeGameMode::CheckFreeForAllAbandon()
{
	// If only one combatant remains connected, they win (others left / never joined).
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::FreeForAll
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	APaintForgePlayerState* Sole = nullptr;
	int32 Count = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS || PS->RosterIndex == 255)
		{
			continue;
		}
		++Count;
		Sole = PS;
	}
	if (Count == 1 && Sole)
	{
		EndFreeForAll(Sole->RosterIndex);
	}
	else if (Count == 0)
	{
		EndFreeForAll(TeamNone);
	}
}

// ---------------------------------------------------------------------------
// Objective match types (CTF / Domination / Hardpoint)
// ---------------------------------------------------------------------------

bool APaintForgeGameMode::IsTeamScoreObjectiveMode(EPFMatchType Type) const
{
	return Type == EPFMatchType::CaptureFlag
		|| Type == EPFMatchType::Domination
		|| Type == EPFMatchType::Hardpoint;
}

void APaintForgeGameMode::EndTeamScoreObjective(uint8 WinnerTeam, const TCHAR* ModeName)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;   // timer + score-cap double-fire guard
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	GetWorldTimerManager().ClearTimer(ObjectiveScoreTimerHandle);
	GetWorldTimerManager().ClearTimer(HardpointRotateTimerHandle);
	ClearAllFlagCarriers();
	DestroyObjectiveActors();
	PendingMatchWinner = WinnerTeam;
	PendingMatchResult = MakeMatchResult(WinnerTeam);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: %s over — winner team %d (%d-%d)"),
		ModeName, WinnerTeam, GS->TeamScores[0], GS->TeamScores[1]);
	SetPhase(EPFMatchPhase::Vote);
}

void APaintForgeGameMode::ResolveCaptureFlagOnTimer()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1]) { Winner = 0; }
	else if (GS->TeamScores[1] > GS->TeamScores[0]) { Winner = 1; }
	EndCaptureFlag(Winner);
}

void APaintForgeGameMode::EndCaptureFlag(uint8 WinnerTeam)
{
	EndTeamScoreObjective(WinnerTeam, TEXT("CaptureFlag"));
}

void APaintForgeGameMode::CheckCaptureFlagAbandon()
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::CaptureFlag
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	const uint8 A = GS->AliveCounts[0];
	const uint8 B = GS->AliveCounts[1];
	if (A > 0 && B > 0) { return; }
	uint8 Winner = TeamNone;
	if (A == 0 && B > 0) { Winner = 1; }
	else if (B == 0 && A > 0) { Winner = 0; }
	EndCaptureFlag(Winner);
}

void APaintForgeGameMode::ResolveDominationOnTimer()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1]) { Winner = 0; }
	else if (GS->TeamScores[1] > GS->TeamScores[0]) { Winner = 1; }
	EndDomination(Winner);
}

void APaintForgeGameMode::EndDomination(uint8 WinnerTeam)
{
	EndTeamScoreObjective(WinnerTeam, TEXT("Domination"));
}

void APaintForgeGameMode::CheckDominationAbandon()
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Domination
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	const uint8 A = GS->AliveCounts[0];
	const uint8 B = GS->AliveCounts[1];
	if (A > 0 && B > 0) { return; }
	uint8 Winner = TeamNone;
	if (A == 0 && B > 0) { Winner = 1; }
	else if (B == 0 && A > 0) { Winner = 0; }
	EndDomination(Winner);
}

void APaintForgeGameMode::ResolveHardpointOnTimer()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1]) { Winner = 0; }
	else if (GS->TeamScores[1] > GS->TeamScores[0]) { Winner = 1; }
	EndHardpoint(Winner);
}

void APaintForgeGameMode::EndHardpoint(uint8 WinnerTeam)
{
	EndTeamScoreObjective(WinnerTeam, TEXT("Hardpoint"));
}

void APaintForgeGameMode::CheckHardpointAbandon()
{
	const APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Hardpoint
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	const uint8 A = GS->AliveCounts[0];
	const uint8 B = GS->AliveCounts[1];
	if (A > 0 && B > 0) { return; }
	uint8 Winner = TeamNone;
	if (A == 0 && B > 0) { Winner = 1; }
	else if (B == 0 && A > 0) { Winner = 0; }
	EndHardpoint(Winner);
}

void APaintForgeGameMode::SpawnObjectiveActors()
{
	UWorld* World = GetWorld();
	APaintForgeGameState* GS = GetPFGameState();
	if (!World || !GS || !HasAuthority())
	{
		return;
	}
	DestroyObjectiveActors();   // idempotent re-entry

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;

	if (GS->MatchType == EPFMatchType::CaptureFlag)
	{
		for (uint8 Team = 0; Team <= 1; ++Team)
		{
			const FVector Home = PFObjectiveLayout::FlagHome(Team);
			APFFlagActor* Flag = World->SpawnActor<APFFlagActor>(
				APFFlagActor::StaticClass(), Home, FRotator::ZeroRotator, Params);
			if (Flag)
			{
				Flag->ServerInit(Team, Home);
				Flags[Team] = Flag;
			}
		}
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: CTF flags spawned at PFGrid homes"));
		return;
	}

	if (GS->MatchType == EPFMatchType::Domination || GS->MatchType == EPFMatchType::Hardpoint)
	{
		const bool bHardpoint = (GS->MatchType == EPFMatchType::Hardpoint);
		ControlPoints.Reset(PFObjectiveLayout::ControlPointCount);
		for (int32 i = 0; i < PFObjectiveLayout::ControlPointCount; ++i)
		{
			const FVector Loc = PFObjectiveLayout::ControlPointLocation(i);
			const bool bActive = !bHardpoint || (i == HardpointActiveSlot);
			APFControlPointActor* CP = World->SpawnActor<APFControlPointActor>(
				APFControlPointActor::StaticClass(), Loc, FRotator::ZeroRotator, Params);
			if (CP)
			{
				CP->ServerInit(i, Loc, bActive);
				ControlPoints.Add(CP);
			}
		}
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: %d control points spawned (%s)"),
			ControlPoints.Num(), bHardpoint ? TEXT("Hardpoint") : TEXT("Domination"));
	}
}

void APaintForgeGameMode::DestroyObjectiveActors()
{
	GetWorldTimerManager().ClearTimer(ObjectiveScoreTimerHandle);
	GetWorldTimerManager().ClearTimer(HardpointRotateTimerHandle);

	for (int32 i = 0; i < 2; ++i)
	{
		if (Flags[i])
		{
			Flags[i]->Destroy();
			Flags[i] = nullptr;
		}
	}
	for (APFControlPointActor* CP : ControlPoints)
	{
		if (CP)
		{
			CP->Destroy();
		}
	}
	ControlPoints.Reset();
}

void APaintForgeGameMode::ClearAllFlagCarriers()
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
			if (PS->bCarryingFlag || PS->StandingOnPoint != 255)
			{
				PS->ServerSetFlagCarry(false, 255);
				PS->ServerSetStandingOnPoint(255);
			}
		}
	}
}

APFFlagActor* APaintForgeGameMode::GetFlagForTeam(uint8 Team) const
{
	if (Team > 1)
	{
		return nullptr;
	}
	return Flags[Team];
}

void APaintForgeGameMode::NotifyFlagTouched(APFFlagActor* Flag, APaintForgePlayerState* Toucher)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || !Flag || !Toucher || !HasAuthority())
	{
		return;
	}
	if (GS->MatchType != EPFMatchType::CaptureFlag
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	if (Toucher->TeamId > 1 || Flag->IsCarried())
	{
		return;
	}

	const uint8 FlagTeam = Flag->GetOwnerTeam();

	// Own flag:
	//  - dropped (not home, not carried) → ally RETURN
	//  - at home + carrying enemy → CAPTURE
	//  - away while still carried by enemy → no-op
	if (FlagTeam == Toucher->TeamId)
	{
		if (!Flag->IsAtHome() && !Flag->IsCarried())
		{
			Flag->ServerReturnHome();
			UE_LOG(PaintForgeLog, Log, TEXT("GameMode: %s returned team %d flag"),
				*Toucher->GetPlayerName(), FlagTeam);
			return;
		}
		if (!Flag->IsAtHome())
		{
			return;   // still out with an enemy carrier — no capture / no return
		}
		if (!Toucher->bCarryingFlag || Toucher->CarriedFlagTeam == Toucher->TeamId)
		{
			return;
		}
		// Capture: score + return both flags + clear carrier.
		APFFlagActor* EnemyFlag = GetFlagForTeam(Toucher->CarriedFlagTeam);
		if (EnemyFlag)
		{
			EnemyFlag->ServerReturnHome();
		}
		Toucher->ServerSetFlagCarry(false, 255);
		Toucher->ServerAddScore(ScoreRoundWin);   // reuse T18 capture bonus weight

		uint16 ScoreA = GS->TeamScores[0];
		uint16 ScoreB = GS->TeamScores[1];
		if (Toucher->TeamId == 0) { ++ScoreA; } else { ++ScoreB; }
		GS->ServerSetTeamScores(ScoreA, ScoreB);
		UE_LOG(PaintForgeLog, Log, TEXT("GameMode: CTF capture by %s (team %d) → %d-%d"),
			*Toucher->GetPlayerName(), Toucher->TeamId, ScoreA, ScoreB);

		if ((Toucher->TeamId == 0 ? ScoreA : ScoreB) >= CaptureFlagTarget)
		{
			EndCaptureFlag(Toucher->TeamId);
		}
		return;
	}

	// Enemy flag (at home or dropped): pick up if not already carrying.
	if (Toucher->bCarryingFlag)
	{
		return;
	}
	Flag->ServerGiveTo(Toucher);
	Toucher->ServerSetFlagCarry(true, FlagTeam);
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: %s picked up team %d flag"),
		*Toucher->GetPlayerName(), FlagTeam);
}

void APaintForgeGameMode::TickDominationScoring()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Domination
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}

	// Clear standing-on-point stamps, then re-stamp from occupancy.
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
		{
			PS->ServerSetStandingOnPoint(255);
		}
	}

	uint16 ScoreA = GS->TeamScores[0];
	uint16 ScoreB = GS->TeamScores[1];
	bool bScored = false;

	for (APFControlPointActor* CP : ControlPoints)
	{
		if (!CP || !CP->IsPointActive())
		{
			continue;
		}
		int32 OutA = 0, OutB = 0;
		const uint8 Sole = CP->ServerQueryOccupancy(OutA, OutB);
		if (Sole <= 1)
		{
			CP->ServerSetControllingTeam(Sole);
		}
		// Contested / empty keeps prior owner (no flip without presence).

		// Stamp PS for anyone currently on this pad (HUD).
		if (OutA + OutB > 0)
		{
			// Occupancy re-query for PS stamp via sphere is already done; walk players near point.
			// Lightweight: any living player whose pawn is within capture radius.
			const FVector CPLoc = CP->GetActorLocation();
			constexpr float RadiusSq = 350.f * 350.f;
			for (APlayerState* PSBase : GS->PlayerArray)
			{
				APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
				if (!PS || !PS->bAliveInRound || PS->TeamId > 1)
				{
					continue;
				}
				if (const APawn* Pawn = PS->GetPawn())
				{
					if (FVector::DistSquared(Pawn->GetActorLocation(), CPLoc) <= RadiusSq)
					{
						PS->ServerSetStandingOnPoint(static_cast<uint8>(CP->GetPointIndex()));
					}
				}
			}
		}

		const uint8 PointOwner = CP->GetControllingTeam();
		if (PointOwner == 0) { ++ScoreA; bScored = true; }
		else if (PointOwner == 1) { ++ScoreB; bScored = true; }
	}

	if (bScored)
	{
		GS->ServerSetTeamScores(ScoreA, ScoreB);
		if (ScoreA >= SkirmishTagTarget)
		{
			EndDomination(0);
			return;
		}
		if (ScoreB >= SkirmishTagTarget)
		{
			EndDomination(1);
		}
	}
}

void APaintForgeGameMode::TickHardpointScoring()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Hardpoint
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}

	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
		{
			PS->ServerSetStandingOnPoint(255);
		}
	}

	APFControlPointActor* Active = nullptr;
	for (APFControlPointActor* CP : ControlPoints)
	{
		if (CP && CP->IsPointActive())
		{
			Active = CP;
			break;
		}
	}
	if (!Active)
	{
		return;
	}

	int32 OutA = 0, OutB = 0;
	const uint8 Sole = Active->ServerQueryOccupancy(OutA, OutB);
	if (Sole <= 1)
	{
		Active->ServerSetControllingTeam(Sole);
	}

	const FVector CPLoc = Active->GetActorLocation();
	constexpr float RadiusSq = 350.f * 350.f;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS || !PS->bAliveInRound || PS->TeamId > 1)
		{
			continue;
		}
		if (const APawn* Pawn = PS->GetPawn())
		{
			if (FVector::DistSquared(Pawn->GetActorLocation(), CPLoc) <= RadiusSq)
			{
				PS->ServerSetStandingOnPoint(static_cast<uint8>(Active->GetPointIndex()));
			}
		}
	}

	const uint8 PointOwner = Active->GetControllingTeam();
	if (PointOwner > 1)
	{
		return;
	}
	uint16 ScoreA = GS->TeamScores[0];
	uint16 ScoreB = GS->TeamScores[1];
	if (PointOwner == 0) { ++ScoreA; } else { ++ScoreB; }
	GS->ServerSetTeamScores(ScoreA, ScoreB);
	if ((PointOwner == 0 ? ScoreA : ScoreB) >= SkirmishTagTarget)
	{
		EndHardpoint(PointOwner);
	}
}

void APaintForgeGameMode::RotateHardpoint()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Hardpoint
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	if (ControlPoints.Num() == 0)
	{
		return;
	}
	HardpointActiveSlot = (HardpointActiveSlot + 1) % ControlPoints.Num();
	for (int32 i = 0; i < ControlPoints.Num(); ++i)
	{
		if (APFControlPointActor* CP = ControlPoints[i])
		{
			CP->ServerSetActive(i == HardpointActiveSlot);
			if (i == HardpointActiveSlot)
			{
				CP->ServerSetControllingTeam(255);   // fresh neutral on rotation
			}
		}
	}
	UE_LOG(PaintForgeLog, Log, TEXT("GameMode: Hardpoint rotated to slot %d"), HardpointActiveSlot);
}

void APaintForgeGameMode::CheckElimVictory()
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	if (RespawnMode == EPFRespawnMode::Respawn
		|| GS->MatchType == EPFMatchType::Skirmish
		|| GS->MatchType == EPFMatchType::FreeForAll
		|| GS->MatchType == EPFMatchType::CaptureFlag
		|| GS->MatchType == EPFMatchType::Domination
		|| GS->MatchType == EPFMatchType::Hardpoint)
	{
		return;   // respawn variant / continuous modes: resolve on the timer / score-cap only
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
		else if (GS->MatchType == EPFMatchType::FreeForAll)
		{
			// Pack top-2 TagCounts into RoundWinsA/B for the JSON record; WinnerTeam = roster or 255.
			uint16 Best = 0, Second = 0;
			for (APlayerState* PSBase : GS->PlayerArray)
			{
				if (const APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase))
				{
					if (PS->TagCount >= Best)
					{
						Second = Best;
						Best = PS->TagCount;
					}
					else if (PS->TagCount > Second)
					{
						Second = PS->TagCount;
					}
				}
			}
			Result.RoundWinsA = static_cast<uint8>(FMath::Min<int32>(Best, 255));
			Result.RoundWinsB = static_cast<uint8>(FMath::Min<int32>(Second, 255));
			Result.RoundsPlayed = 1;
		}
		else if (IsTeamScoreObjectiveMode(GS->MatchType))
		{
			// CTF / Dom / HP: TeamScores are captures or control points.
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
	Result.bSuddenDeath = bSuddenDeathPlayed;   // always false in Skirmish / FFA
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

	// FREE-FOR-ALL: credit the SHOOTER a personal tag (score/elims already booked above), check cap, respawn.
	// Unique combat TeamIds (= roster) so B12 never treats two players as teammates.
	if (GS->MatchType == EPFMatchType::FreeForAll)
	{
		if (ShooterPS && ShooterPS != VictimPS)
		{
			ShooterPS->ServerAddTag();
			if (ShooterPS->TagCount >= SkirmishTagTarget)
			{
				EndFreeForAll(ShooterPS->RosterIndex);
				return;
			}
		}
		RespawnVictimAtTeamSpawn(Victim);
		return;
	}

	// CAPTURE THE FLAG: carrier death DROPS the flag (auto-return timer on the actor); ally can
	// return it early by touch. No tag scoring; respawn like Skirmish.
	if (GS->MatchType == EPFMatchType::CaptureFlag)
	{
		if (VictimPS->bCarryingFlag)
		{
			if (APFFlagActor* Carried = GetFlagForTeam(VictimPS->CarriedFlagTeam))
			{
				Carried->ServerDropAt(Victim->GetActorLocation());
			}
			VictimPS->ServerSetFlagCarry(false, 255);
		}
		RespawnVictimAtTeamSpawn(Victim);
		return;
	}

	// DOMINATION / HARDPOINT: elims for stats only; control-point scoring is the timer tick.
	if (GS->MatchType == EPFMatchType::Domination || GS->MatchType == EPFMatchType::Hardpoint)
	{
		VictimPS->ServerSetStandingOnPoint(255);
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
			PS->TagCount = 0;
			PS->MatchScore = 0;
			PS->bHasVoted = false;
			PS->ServerSetFlagCarry(false, 255);
			PS->ServerSetStandingOnPoint(255);
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

bool APaintForgeGameMode::IsActiveRosterMember(const APaintForgePlayerState* PS)
{
	if (!PS || !IsValid(PS))
	{
		return false;
	}
	// Bots always count while their AI controller owns them. Humans without a Controller are ghosts.
	if (PS->IsABot())
	{
		return PS->GetOwningController() != nullptr;
	}
	return PS->GetPlayerController() != nullptr || PS->GetOwningController() != nullptr;
}

void APaintForgeGameMode::ScrubGhostPlayerStates(const APaintForgePlayerState* KeepPS)
{
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS || !HasAuthority())
	{
		return;
	}
	TArray<APaintForgePlayerState*> ToKill;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (!PS || PS == KeepPS || PS->IsABot())
		{
			continue;
		}
		if (!IsActiveRosterMember(PS))
		{
			ToKill.Add(PS);
		}
	}
	for (APaintForgePlayerState* Ghost : ToKill)
	{
		UE_LOG(PaintForgeLog, Warning, TEXT("GameMode: scrubbing ghost PlayerState %s"),
			*Ghost->GetPlayerName());
		if (TObjectPtr<APFTargetDummy>* Dummy = WarmupDummies.Find(Ghost))
		{
			if (*Dummy) { (*Dummy)->Destroy(); }
			WarmupDummies.Remove(Ghost);
		}
		Ghost->Destroy();
	}
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
		if (!PS || !IsActiveRosterMember(PS))
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
		if (!PS || PS == IgnorePS || PS->IsABot() || !IsActiveRosterMember(PS))
		{
			continue;   // bots never ready; ghosts never block Ready
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
				if (PS && IsActiveRosterMember(PS) && PS->RosterIndex == Candidate)
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
	// Continuous timed modes: bypass Elimination round scaling. RoundWinsToTake carries the score
	// TARGET for the HUD (CTF uses CaptureFlagTarget; others use SkirmishTagTarget).
	if (APaintForgeGameState* SkGS = GetPFGameState())
	{
		if (SkGS->MatchType == EPFMatchType::Skirmish
			|| SkGS->MatchType == EPFMatchType::FreeForAll
			|| IsTeamScoreObjectiveMode(SkGS->MatchType))
		{
			const int32 Target = (SkGS->MatchType == EPFMatchType::CaptureFlag)
				? static_cast<int32>(CaptureFlagTarget)
				: static_cast<int32>(SkirmishTagTarget);
			const uint8 TargetU8 = static_cast<uint8>(FMath::Min(Target, 255));
			EffectiveRoundWinsToTake = TargetU8;
			EffectiveMaxRounds = 1;
			EffectiveRoundDuration = SkirmishMatchDuration;
			SkGS->ServerSetRoundWinsToTake(TargetU8);
			UE_LOG(PaintForgeLog, Log, TEXT("GameMode: match type %d — first to %d, %.0f s"),
				static_cast<int32>(SkGS->MatchType), Target, SkirmishMatchDuration);
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
