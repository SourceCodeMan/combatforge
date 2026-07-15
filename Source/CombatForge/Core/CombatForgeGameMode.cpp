// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/CombatForgeGameMode.h"

#include "CombatForge.h"
#include "Core/PFPaths.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerController.h"
#include "Core/CombatForgePlayerState.h"
#include "Player/CombatForgeCharacter.h"
#include "AI/PFBotController.h"
#include "Combat/PFHealthComponent.h"
#include "Combat/PFWeaponComponent.h"
#include "Combat/PFTargetDummy.h"
#include "Combat/PFAmmoBarrel.h"
#include "Building/PFArenaShell.h"
#include "Building/PFYardShell.h"
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

ACombatForgeGameMode::ACombatForgeGameMode()
{
	DefaultPawnClass      = ACombatForgeCharacter::StaticClass();
	PlayerControllerClass = ACombatForgePlayerController::StaticClass();
	GameStateClass        = ACombatForgeGameState::StaticClass();
	PlayerStateClass      = ACombatForgePlayerState::StaticClass();
	bUseSeamlessTravel = false;   // single persistent level, no travel (02 D1)
}

// ---------------------------------------------------------------------------
// World bootstrap
// ---------------------------------------------------------------------------

void ACombatForgeGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
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
			UE_LOG(CombatForgeLog, Log, TEXT("GameMode: net timeouts Connection=%.0fs Initial=%.0fs"),
				Net->ConnectionTimeout, Net->InitialConnectTimeout);
		}
	}
}

void ACombatForgeGameMode::BeginPlay()
{
	Super::BeginPlay();
	SpawnArenaActors();

	// First install / packaged playtest: ensure starter community maps exist.
	if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
	{
		Rating->EnsureSeedArenas();
	}

	// NetDriver is often created after InitGame for listen hosts — re-apply timeouts here.
	if (UWorld* World = GetWorld())
	{
		if (UNetDriver* Net = World->GetNetDriver())
		{
			Net->ConnectionTimeout = 12.f;
			Net->InitialConnectTimeout = 20.f;
		}
	}

	// Crash triage breadcrumb: periodic roster dump during combat so host logs show who was
	// connected in the minute before a client vanished (kids "crash with no dialog").
	GetWorldTimerManager().SetTimer(CrashBreadcrumbTimer, this,
		&ACombatForgeGameMode::LogCrashBreadcrumb, 30.f, /*bLoop=*/true);

	EffectiveRoundWinsToTake = RoundWinsToTakeMatch;
	EffectiveMaxRounds = MaxRounds;
	EffectiveRoundDuration = RoundDuration;

	if (ACombatForgeGameState* GS = GetPFGameState())
	{
		GS->ServerSetTargetTeamSize(DefaultTeamSize);   // 4v4 default; host can switch to 6v6 on boot menu
		GS->ServerSetFillWithBots(bFillWithBots);       // bots on by default; host can disable on boot menu
		GS->ServerSetBuildMode(DefaultBuildMode);       // Creative default; host picks on boot menu
		GS->ServerSetMatchType(DefaultMatchType);       // Skirmish default (kids)
		GS->ServerSetArenaMap(DefaultArenaMap);         // Warehouse default; must match SpawnArenaActors' pick
	}

#if !UE_BUILD_SHIPPING
	// Deploy/playtest/smoke-improvement.ps1: editor -game -nullrhi -SmokeImprovement
	if (FParse::Param(FCommandLine::Get(), TEXT("SmokeImprovement")))
	{
		bFillWithBots = true;   // need two sides for a normal lobby→build
		SmokeImprovementStep = 0;
		SmokeImprovementElapsed = 0.f;
		GetWorldTimerManager().SetTimer(SmokeImprovementTimer, this,
			&ACombatForgeGameMode::TickSmokeImprovement, 0.5f, true);
		UE_LOG(CombatForgeLog, Warning, TEXT("SMOKE: Improvement path armed (-SmokeImprovement)"));
	}
#endif
}

#if !UE_BUILD_SHIPPING
namespace
{
	/** Write a tiny both-team fort into the stable arena dir so packaged smokes don't depend on host paths. */
	bool SmokeWriteCommunityArenaSeed()
	{
		const FString Dir = FPFPaths::ArenaDir();
		const FString Path = Dir / TEXT("arena_99999999_smokeseed.json");
		// Newest-wins loader sorts by name — high timestamp prefix stays on top of empty match dumps.
		const TCHAR* Json =
			TEXT("{\n")
			TEXT("  \"schema\": 1,\n")
			TEXT("  \"game\": \"CombatForge\",\n")
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
			UE_LOG(CombatForgeLog, Error, TEXT("SMOKE: failed to write arena seed %s"), *Path);
			return false;
		}
		UE_LOG(CombatForgeLog, Warning, TEXT("SMOKE: wrote community seed %s (ProjectSavedDir=%s)"),
			*Path, *FPaths::ProjectSavedDir());
		return true;
	}
}

void ACombatForgeGameMode::TickSmokeImprovement()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	SmokeImprovementElapsed += 0.5f;

	// Hard timeout — never hang a CI/playtest host.
	if (SmokeImprovementElapsed > 90.f)
	{
		UE_LOG(CombatForgeLog, Error, TEXT("SMOKE: Improvement FAIL (timeout %.0fs, phase=%d, basePieces=%d)"),
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
				if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(It->Get()))
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
		UE_LOG(CombatForgeLog, Warning, TEXT("SMOKE: configured Improvement + Skirmish 2v2"));
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
			UE_LOG(CombatForgeLog, Warning, TEXT("SMOKE: HostForceStart (lobby countdown)"));
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
				UE_LOG(CombatForgeLog, Warning,
					TEXT("SMOKE: Improvement PASS (CommunityBasePieces=%d, phase=%d)"),
					N, static_cast<int32>(GS->Phase));
			}
			else
			{
				UE_LOG(CombatForgeLog, Error,
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

namespace
{
	/** Map → shell class. Class identity IS how the map reaches clients (ctor-built geometry). */
	UClass* PFShellClassForMap(EPFArenaMap Map)
	{
		return (Map == EPFArenaMap::Yard)
			? APFYardShell::StaticClass()
			: APFArenaShell::StaticClass();
	}
}

void ACombatForgeGameMode::SpawnArenaActors()
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
	// The shell class follows the selected map (HostSetArenaMap swaps it live in the lobby).
	ArenaShell = World->SpawnActor<APFArenaShell>(PFShellClassForMap(DefaultArenaMap), FTransform::Identity, Params);
	BuildGrid  = World->SpawnActor<APFBuildGrid>(APFBuildGrid::StaticClass(), FTransform::Identity, Params);

	// The lighting rig is spawned per-machine by UPFLightingSubsystem (host AND every remote client) so
	// clients aren't left with an unlit scene — the server-only GameMode must not own render-only actors.

	if (AWorldSettings* WorldSettings = World->GetWorldSettings())
	{
		WorldSettings->KillZ = -1000.f;
	}

	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: arena shell and build grid spawned"));
}

// ---------------------------------------------------------------------------
// Login / logout / spawning
// ---------------------------------------------------------------------------

void ACombatForgeGameMode::PostLogin(APlayerController* NewPlayer)
{
	// Drop any human PlayerStates that lost their controller (ghosts from crash/force-quit).
	// Must run before team assignment so roster slots free up for the rejoiner.
	ScrubGhostPlayerStates(/*KeepPS=*/nullptr);

	// Assign team + roster slot BEFORE Super::PostLogin so the initial pawn spawn already
	// knows its side (RestartPlayer → GetSpawnTransform reads TeamId).
	ACombatForgePlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	if (PS && PS->TeamId == TeamNone)
	{
		const ACombatForgeGameState* PreGS = GetPFGameState();
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

		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: %s joined (team %d, roster %d)"),
			*PS->GetPlayerName(), PS->TeamId, PS->RosterIndex);
	}
}

void ACombatForgeGameMode::Logout(AController* Exiting)
{
	ACombatForgePlayerState* ExitingPS =
		Exiting ? Exiting->GetPlayerState<ACombatForgePlayerState>() : nullptr;
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
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: %s left"), *ExitingPS->GetPlayerName());
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

void ACombatForgeGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer || NewPlayer->GetPawn())
	{
		return;
	}
	const ACombatForgePlayerState* PS = NewPlayer->GetPlayerState<ACombatForgePlayerState>();
	if (!PS)
	{
		return;
	}

	RestartPlayerAtTransform(NewPlayer, GetSpawnTransform(PS));

	if (ACombatForgeCharacter* Pawn = Cast<ACombatForgeCharacter>(NewPlayer->GetPawn()))
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
			Health->OnEliminatedEvent.AddUObject(this, &ACombatForgeGameMode::HandlePawnHealthEliminated);
		}
	}
}

void ACombatForgeGameMode::HandlePawnHealthEliminated(UPFHealthComponent* Health, const FPFPaintHitInfo& FinalHit)
{
	ACombatForgeCharacter* Victim = Health ? Cast<ACombatForgeCharacter>(Health->GetOwner()) : nullptr;
	if (Victim)
	{
		NotifyPawnEliminated(Victim, FinalHit);
	}
}

void ACombatForgeGameMode::SpawnWarmupDummyFor(ACombatForgePlayerState* PS)
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

FTransform ACombatForgeGameMode::GetSpawnTransform(const ACombatForgePlayerState* PS) const
{
	if (!PS || !ArenaShell)
	{
		return FTransform(FVector(0.f, 0.f, 200.f));
	}

	const ACombatForgeGameState* GS = GetPFGameState();
	const EPFMatchPhase Phase = GS ? GS->Phase : EPFMatchPhase::Lobby;

	// FreeForAll: combat = random points all over the field (not just spawn columns).
	if (GS && GS->MatchType == EPFMatchType::FreeForAll)
	{
		if (Phase == EPFMatchPhase::Lobby)
		{
			// Lobby = freeform warmup ON THE FIELD (the old detached warm-up pen kept everyone boxed in a
			// 20x20m island). Spawn columns are never buildable, so leftover forts can't embed a spawn.
			return ArenaShell->GetTeamSpawnTransform(PS->RosterIndex % 2, GetTeamSlotIndex(PS) % PFGrid::SpawnPointsPerTeam);
		}
		// Salt mixes roster + round + a rolling counter so respawns don't stack.
		const int32 Salt = static_cast<int32>(PS->RosterIndex) * 97
			+ static_cast<int32>(GS->RoundNumber) * 131
			+ static_cast<int32>(FMath::FloorToInt(GetWorld() ? GetWorld()->GetTimeSeconds() * 1000.f : 0.f));
		return ArenaShell->GetRandomFieldSpawnTransform(Salt);
	}

	const uint8 Team = (PS->TeamId <= 1) ? PS->TeamId : 0;
	const int32 TeamSlot = GetTeamSlotIndex(PS);

	switch (Phase)
	{
	case EPFMatchPhase::Lobby:
		// Lobby = freeform warmup on the field: run around, shoot, nothing counts. (Was the detached pen.)
		return ArenaShell->GetTeamSpawnTransform(Team, TeamSlot % PFGrid::SpawnPointsPerTeam);

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

int32 ACombatForgeGameMode::GetTeamSlotIndex(const ACombatForgePlayerState* PS) const
{
	// Ordinal among same-team players sorted by RosterIndex — deterministic and index-stable.
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || !PS)
	{
		return 0;
	}
	int32 Slot = 0;
	for (APlayerState* Other : GS->PlayerArray)
	{
		const ACombatForgePlayerState* OtherPS = Cast<ACombatForgePlayerState>(Other);
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

void ACombatForgeGameMode::SetPhase(EPFMatchPhase NewPhase)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || !HasAuthority())
	{
		return;
	}
	const EPFMatchPhase OldPhase = GS->Phase;

	// Every transition cancels pending phase machinery.
	GetWorldTimerManager().ClearTimer(PhaseTimerHandle);
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	GetWorldTimerManager().ClearTimer(LobbyCountdownHandle);
	GetWorldTimerManager().ClearTimer(LobbyTopUpHandle);
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

	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: phase %d -> %d"),
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
			ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
			if (!PS)
			{
				continue;
			}
			PS->ServerSetReady(false);
			PS->bAliveInRound = true;
			if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(PS->GetPlayerController()))
			{
				PC->SetEliminatedMoveLock(false);
			}
			RespawnCombatant(PS, 3);   // players and bots alike
		}
		RecountAlive();
		// Freeform warmup: top every combatant's loadout up every couple of seconds so lobby play is
		// effectively unlimited ammo/grenades (ServerRefillFromPickup no-ops when already full). Cleared on
		// any phase transition, and the real match wipes loadouts fresh anyway (RespawnCombatant).
		GetWorldTimerManager().SetTimer(LobbyTopUpHandle, this, &ACombatForgeGameMode::TopUpLobbyLoadouts, 2.f, true);
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
				if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
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
			ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
			if (!PS)
			{
				continue;
			}
			PS->ServerSetBudgets(BudgetStructural, BudgetProps);
			PS->ServerSetReady(false);
			PS->bAliveInRound = true;
			if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(PS->GetPlayerController()))
			{
				PC->SetEliminatedMoveLock(false);
			}
			RespawnCombatant(PS, 3);   // own plot; players and bots alike
		}
		RecountAlive();

		// Community inject (after bots exist, grid cleared, before FreezeBuild) so it replicates
		// through the build window and is captured by BeginMatchRecord at Combat.
		//   Improvement / Play-only → whole community arena (host-picked or top-ranked).
		//   Creative               → only an all-bot team's half is filled.
		if (BuildGrid)
		{
			// Remix lineage: cleared each build; set only when a community base is actually loaded below. A
			// Creative / from-scratch build leaves it empty so its save records no parent (see BeginMatchRecord).
			PendingParentArenaId.Reset();
			if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
			{
				if (GS->BuildMode == EPFBuildMode::Improvement || GS->BuildMode == EPFBuildMode::PlayOnly)
				{
					TArray<FPFBuildPieceRec> Whole;
					FString BaseArenaId;
					if (Rating->PickCommunityArena(Whole, BaseArenaId, GS->SelectedCommunityMapFile))
					{
						BuildGrid->ServerInjectPieces(Whole);
						PendingParentArenaId = BaseArenaId;   // source map this Remix builds on; captured at save
						UE_LOG(CombatForgeLog, Log,
							TEXT("GameMode: %s loaded %d community pieces (map=%s, arenaId=%s)"),
							GS->BuildMode == EPFBuildMode::PlayOnly ? TEXT("PlayOnly") : TEXT("Improvement"),
							Whole.Num(),
							GS->SelectedCommunityMapFile.IsEmpty() ? TEXT("auto") : *GS->SelectedCommunityMapFile,
							*BaseArenaId.Left(8));
					}
					else
					{
						UE_LOG(CombatForgeLog, Warning,
							TEXT("GameMode: community mode — no arena on disk; starting empty"));
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
			&ACombatForgeGameMode::StartNextRoundFromBuildEnd,
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
				// PendingParentArenaId is non-empty only for a Remix of a loaded community map; BeginMatchRecord
				// drops it when the frozen layout hashes identically (nothing changed ⇒ same map, no fork).
				Rating->BeginMatchRecord(GS->MatchId, BuildGrid->GetPieces(), FMath::Max(TeamA, TeamB),
					PendingParentArenaId);
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
		// Objective actors + ammo barrels live only during combat Live — tear them down.
		DestroyObjectiveActors();
		DestroyAmmoBarrels();
		GS->ServerSetRoundState(EPFRoundState::None, 0.f);
		GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
			&ACombatForgeGameMode::FinalizeVotePhase, VotePhaseDuration, false);
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
			FTimerDelegate::CreateUObject(this, &ACombatForgeGameMode::SetPhase, EPFMatchPhase::Lobby),
			ResultsDuration, false);
		break;
	}
	}

	ApplyServerMoveLocks();
}

// ---------------------------------------------------------------------------
// Lobby / Build ready flow
// ---------------------------------------------------------------------------

void ACombatForgeGameMode::NotifyReadyChanged()
{
	NotifyReadyChangedInternal(nullptr);
}

void ACombatForgeGameMode::NotifyReadyChangedInternal(const ACombatForgePlayerState* IgnorePS)
{
	ACombatForgeGameState* GS = GetPFGameState();
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
				if (LobbyStartCountdown <= 0.f)
				{
					StartNextRoundFromBuildEnd();   // instant end — no stray 0-second one-shot timer
				}
				else
				{
					GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
						&ACombatForgeGameMode::StartNextRoundFromBuildEnd, LobbyStartCountdown, false);
				}
				UE_LOG(CombatForgeLog, Log, TEXT("GameMode: all ready - build phase ending in %.0f s"), LobbyStartCountdown);
			}
		}
		else if (!bAllReady && bBuildEarlyEndActive)
		{
			bBuildEarlyEndActive = false;
			const float Restored = FMath::Max(BuildPhaseFullEndTime - Now, 0.1f);
			GS->ServerSetPhaseEndTime(Now + Restored);
			GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
				&ACombatForgeGameMode::StartNextRoundFromBuildEnd, Restored, false);
		}
	}
}

void ACombatForgeGameMode::BeginLobbyStartCountdown(bool bForced)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || bLobbyCountdownActive)
	{
		return;
	}
	if (LobbyStartCountdown <= 0.f)
	{
		// No pre-match grace — go straight to Build (Combat then supplies the single per-round "GET READY" Freeze).
		SetPhase(EPFMatchPhase::Build);
		return;
	}
	bLobbyCountdownActive = true;
	bLobbyCountdownForced = bForced;
	GS->ServerSetPhaseEndTime(GS->GetServerWorldTimeSeconds() + LobbyStartCountdown);
	GetWorldTimerManager().SetTimer(LobbyCountdownHandle,
		FTimerDelegate::CreateUObject(this, &ACombatForgeGameMode::SetPhase, EPFMatchPhase::Build),
		LobbyStartCountdown, false);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: match starting in %.0f s (%s)"),
		LobbyStartCountdown, bForced ? TEXT("host force") : TEXT("all ready"));
}

void ACombatForgeGameMode::CancelLobbyStartCountdown()
{
	if (!bLobbyCountdownActive)
	{
		return;
	}
	bLobbyCountdownActive = false;
	bLobbyCountdownForced = false;
	GetWorldTimerManager().ClearTimer(LobbyCountdownHandle);
	if (ACombatForgeGameState* GS = GetPFGameState())
	{
		GS->ServerSetPhaseEndTime(0.f);
	}
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: lobby start countdown cancelled"));
}

void ACombatForgeGameMode::HostForceStart()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;
	}
	BeginLobbyStartCountdown(/*bForced=*/true);
}

void ACombatForgeGameMode::HostCycleTeam(ACombatForgePlayerState* Target)
{
	ACombatForgeGameState* GS = GetPFGameState();
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
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host cycle refused - team %d already at format size"), NewTeam);
		return;
	}
	Target->ServerSetTeam(NewTeam, Target->RosterIndex);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host moved %s to team %d"), *Target->GetPlayerName(), NewTeam);
}

void ACombatForgeGameMode::HostReturnToLobby()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Results)
	{
		return;
	}
	SetPhase(EPFMatchPhase::Lobby);
}

void ACombatForgeGameMode::HostForceReturnToLobby()
{
	// Mid-match quit-to-menu (host): tear down combat toys and jump straight to Lobby.
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase == EPFMatchPhase::Lobby)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(PhaseTimerHandle);
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	GetWorldTimerManager().ClearTimer(LobbyCountdownHandle);
	GetWorldTimerManager().ClearTimer(ObjectiveScoreTimerHandle);
	GetWorldTimerManager().ClearTimer(HardpointRotateTimerHandle);
	DestroyObjectiveActors();
	DestroyAmmoBarrels();
	RemoveAllBots();
	SetPhase(EPFMatchPhase::Lobby);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host force-returned to Lobby (quit to menu)"));
}

void ACombatForgeGameMode::HostSetFormat(uint8 NewTeamSize)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;   // format locks once the match starts (bots + scaling resolve at Lobby→Build)
	}
	// Only 4v4 or 6v6 from the host UI (internal smoke can still use smaller sizes via other paths).
	const uint8 Clamped = (NewTeamSize >= 6) ? 6 : 4;
	GS->ServerSetTargetTeamSize(Clamped);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host set format to %dv%d"),
		GS->TargetTeamSize, GS->TargetTeamSize);
}

void ACombatForgeGameMode::HostSetFillWithBots(bool bFill)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;
	}
	bFillWithBots = bFill;
	GS->ServerSetFillWithBots(bFill);
	// If bots were already in the roster (e.g. host toggled off after a previous fill attempt), clear them.
	if (!bFill)
	{
		RemoveAllBots();
	}
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host set fill-with-bots = %s"), bFill ? TEXT("true") : TEXT("false"));
}

void ACombatForgeGameMode::HostSetBuildMode(EPFBuildMode NewMode)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || NewMode >= EPFBuildMode::MAX_Count)
	{
		return;
	}
	// FreeForAll is play-only by design — Creative/Improvement would flash past or confuse the lobby.
	if (GS->MatchType == EPFMatchType::FreeForAll && NewMode != EPFBuildMode::PlayOnly)
	{
		NewMode = EPFBuildMode::PlayOnly;
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: FreeForAll forces PlayOnly build mode"));
	}
	GS->ServerSetBuildMode(NewMode);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host set build mode %d"), static_cast<int32>(NewMode));
}

void ACombatForgeGameMode::HostSetMatchType(EPFMatchType NewType)
{
	ACombatForgeGameState* GS = GetPFGameState();
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
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host set match type %d"), static_cast<int32>(NewType));
}

void ACombatForgeGameMode::HostSetArenaMap(EPFArenaMap NewMap)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby || NewMap >= EPFArenaMap::MAX_Count)
	{
		return;   // the field can only change while nothing is built on it (same gate as the other HostSet*)
	}
	// ApplySelectionsToHost re-sends every selector on any card click — don't tear the arena down
	// (and re-seat everyone) unless the map actually changed.
	if (GS->ArenaMap == NewMap && ArenaShell)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	GS->ServerSetArenaMap(NewMap);

	// Map identity travels as ACTOR CLASS: destroy the old shell and spawn the new map's class —
	// the actor channel tears down / constructs the ctor-built geometry on every client, so the
	// swap needs zero new replication machinery (the shell contract: "replicated for existence
	// only, geometry ctor-built identically everywhere").
	if (ArenaShell)
	{
		ArenaShell->Destroy();
		ArenaShell = nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ArenaShell = World->SpawnActor<APFArenaShell>(PFShellClassForMap(NewMap), FTransform::Identity, Params);

	// Re-seat every combatant on the new field: a Yard→Warehouse shrink can leave lobby-warmup
	// pawns standing outside the new perimeter. Same reseat path SetPhase(Lobby) uses — teleport
	// a live pawn to its (new-shell) spawn, or RestartPlayer when it has none.
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
		{
			RespawnCombatant(PS, 3);
		}
	}
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host set arena map %d (%s) — shell respawned, players re-seated"),
		static_cast<int32>(NewMap), *PFGetArenaMapDef(NewMap).Label);
}

void ACombatForgeGameMode::HostSetCommunityMap(const FString& FileName, const FString& Label)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;
	}
	// Empty FileName = auto (top-ranked). Non-empty must exist on disk.
	if (!FileName.IsEmpty())
	{
		if (UPFRatingSubsystem* Rating = GetRatingSubsystem())
		{
			TArray<FPFBuildPieceRec> Probe;
			if (!Rating->LoadCommunityArenaByFileName(FileName, Probe))
			{
				UE_LOG(CombatForgeLog, Warning,
					TEXT("GameMode: host community map rejected (missing/bad): %s"), *FileName);
				return;
			}
		}
	}
	GS->ServerSetSelectedCommunityMap(FileName, Label);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: host set community map '%s' (%s)"),
		FileName.IsEmpty() ? TEXT("auto") : *FileName, *Label);
}

// ---------------------------------------------------------------------------
// Round loop
// ---------------------------------------------------------------------------

void ACombatForgeGameMode::StartNextRoundFromBuildEnd()
{
	// Build timer / early-end countdown fired: unspent budget is discarded implicitly.
	SetPhase(EPFMatchPhase::Combat);
}

void ACombatForgeGameMode::StartNextRound()
{
	ACombatForgeGameState* GS = GetPFGameState();
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
		ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		PS->bAliveInRound = true;
		if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(PS->GetPlayerController()))
		{
			PC->SetEliminatedMoveLock(false);   // back alive; freeze-state lock reapplies below
		}
		RespawnCombatant(PS, RoundHP);          // players and bots alike
		if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(PS->GetPlayerController()))
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
		}
	}
	RecountAlive();

	GS->ServerSetRoundState(EPFRoundState::Freeze, GS->GetServerWorldTimeSeconds() + FreezeDuration);
	ApplyServerMoveLocks();
	GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
		&ACombatForgeGameMode::BeginLiveRound, FreezeDuration, false);

	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: round %d freeze (%s)"),
		GS->RoundNumber, bSuddenDeathRoundActive ? TEXT("SHOWDOWN") : TEXT("normal"));
}

void ACombatForgeGameMode::ResetPawnForRound(ACombatForgeCharacter* Pawn, ACombatForgePlayerState* PS, uint8 RoundHP)
{
	if (PS)
	{
		PS->ServerClearOutState();
	}
	if (!Pawn || !PS)
	{
		return;
	}
	Pawn->GetHealth()->ResetForRound(RoundHP);   // restores HP, collision, appearance
	TeleportPawnTo(Pawn, GetSpawnTransform(PS));
}

void ACombatForgeGameMode::TeleportPawnTo(ACombatForgeCharacter* Pawn, const FTransform& Transform)
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

void ACombatForgeGameMode::RespawnCombatant(ACombatForgePlayerState* PS, uint8 RoundHP)
{
	// Controller-agnostic: works for a human PlayerController AND a bot AIController. The pawn is found
	// via the PlayerState (PS->GetPawn), and a missing pawn is restarted via PS->GetOwningController so
	// bots go through the same RestartPlayer path humans do (which also binds their elimination event).
	if (!PS)
	{
		return;
	}
	PS->ServerClearOutState();
	if (ACombatForgeCharacter* Pawn = Cast<ACombatForgeCharacter>(PS->GetPawn()))
	{
		if (UPFHealthComponent* Health = Pawn->GetHealth())
		{
			Health->ResetForRound(RoundHP);   // restores HP, collision, appearance
		}
		if (UPFWeaponComponent* Weapon = Pawn->GetWeapon())
		{
			Weapon->ServerResetLoadout();     // corpse-reused pawn: refill mag + reserve + grenades
		}
		TeleportPawnTo(Pawn, GetSpawnTransform(PS));
	}
	else if (AController* Ctrl = PS->GetOwningController())
	{
		RestartPlayer(Ctrl);
		if (ACombatForgeCharacter* NewPawn = Cast<ACombatForgeCharacter>(Ctrl->GetPawn()))
		{
			if (UPFHealthComponent* Health = NewPawn->GetHealth())
			{
				Health->ResetForRound(RoundHP);
			}
			if (UPFWeaponComponent* Weapon = NewPawn->GetWeapon())
			{
				Weapon->ServerResetLoadout();
			}
		}
	}
}

void ACombatForgeGameMode::RespawnVictimAtTeamSpawn(ACombatForgeCharacter* Victim, float DelaySec)
{
	if (!Victim)
	{
		return;
	}
	const float Delay = (DelaySec >= 0.f) ? DelaySec : RespawnDelay;
	// Timed reset-in-place (Skirmish + the Respawn variant): the victim is NOT marked dead, move-locked,
	// or death-cammed — after Delay it heals to full and teleports to its team spawn. PS is
	// re-fetched inside the timer via the weak victim (safe if it despawned). Works for players and bots.
	// Stamp OutKind + RespawnAtServerTime so the victim's HUD can show "YOU'RE OUT" + countdown.
	if (ACombatForgePlayerState* VictimPS = Victim->GetPlayerState<ACombatForgePlayerState>())
	{
		float At = Delay;
		if (const ACombatForgeGameState* GS = GetPFGameState())
		{
			At = GS->GetServerWorldTimeSeconds() + Delay;
		}
		VictimPS->ServerSetOutWaitingRespawn(At);
	}

	TWeakObjectPtr<ACombatForgeCharacter> WeakVictim(Victim);
	TWeakObjectPtr<ACombatForgePlayerState> WeakVictimPS(Victim->GetPlayerState<ACombatForgePlayerState>());
	TWeakObjectPtr<ACombatForgeGameMode> WeakThis(this);
	FTimerHandle RespawnHandle;
	GetWorldTimerManager().SetTimer(RespawnHandle,
		FTimerDelegate::CreateLambda([WeakThis, WeakVictim, WeakVictimPS]()
		{
			if (!WeakThis.IsValid())
			{
				return;
			}
			if (WeakVictim.IsValid())
			{
				// Pawn survived the out window — reset it in place.
				if (ACombatForgePlayerState* PS = WeakVictim->GetPlayerState<ACombatForgePlayerState>())
				{
					PS->ServerClearOutState();
				}
				WeakVictim->GetHealth()->ResetForRound(3);
				// Reused pawn keeps its depleted ammo/grenades unless we reset — every respawn grants a FULL
				// loadout (mag + reserve + both grenades; also clears an in-flight reload). Fresh-pawn paths
				// already fill in the weapon's BeginPlay.
				if (UPFWeaponComponent* Weapon = WeakVictim->GetWeapon())
				{
					Weapon->ServerResetLoadout();
				}
				if (ACombatForgePlayerState* PS = WeakVictim->GetPlayerState<ACombatForgePlayerState>())
				{
					WeakThis->TeleportPawnTo(WeakVictim.Get(), WeakThis->GetSpawnTransform(PS));
				}
			}
			else if (WeakVictimPS.IsValid())
			{
				// Pawn was destroyed during the out window (e.g. a still-driven bot corpse fell past KillZ) —
				// restart via the controller so the player/bot returns instead of freezing out for the match.
				ACombatForgePlayerState* PS = WeakVictimPS.Get();
				PS->ServerClearOutState();
				if (AController* C = PS->GetOwningController())
				{
					WeakThis->RestartPlayer(C);
				}
			}
		}),
		FMath::Max(0.05f, Delay), false);
}

// ---------------------------------------------------------------------------
// Bots (fill teams to the selected format — server only)
// ---------------------------------------------------------------------------

int32 ACombatForgeGameMode::GetTeamCountByKind(uint8 Team, bool bBotsOnly) const
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return 0;
	}
	int32 Count = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
		if (PS && PS->TeamId == Team && (!bBotsOnly || PS->IsABot()))
		{
			++Count;
		}
	}
	return Count;
}

ACombatForgePlayerState* ACombatForgeGameMode::AddBot(uint8 Team)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	// Team modes: Team must be 0/1. FreeForAll ignores Team and assigns a unique combat id.
	const ACombatForgeGameState* GS = GetPFGameState();
	const bool bFFA = GS && GS->MatchType == EPFMatchType::FreeForAll;
	if (!bFFA && Team > 1)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// bWantsPlayerState=true (ctor) => the controller's PostInitializeComponents already created and
	// registered an ACombatForgePlayerState in GameState->PlayerArray during SpawnActor.
	APFBotController* Bot = World->SpawnActor<APFBotController>(APFBotController::StaticClass(),
		FTransform::Identity, Params);
	ACombatForgePlayerState* PS = Bot ? Bot->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	if (!PS)
	{
		if (Bot) { Bot->Destroy(); }
		UE_LOG(CombatForgeLog, Warning, TEXT("GameMode: AddBot failed (no PlayerState)"));
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
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: added bot '%s' (team %d, roster %d)"),
		*PS->GetPlayerName(), PS->TeamId, PS->RosterIndex);
	return PS;
}

void ACombatForgeGameMode::TopUpLobbyLoadouts()
{
	// Freeform warmup: unlimited ammo/grenades while in Lobby. ServerRefillFromPickup no-ops when full, so
	// this is cheap; the timer is cleared on any phase transition (belt: bail if the phase moved on).
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Lobby)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
		ACombatForgeCharacter* Char = PS ? Cast<ACombatForgeCharacter>(PS->GetPawn()) : nullptr;
		if (UPFWeaponComponent* Weapon = Char ? Char->GetWeapon() : nullptr)
		{
			Weapon->ServerRefillFromPickup();
		}
	}
}

void ACombatForgeGameMode::FillBotsToFormat()
{
	ACombatForgeGameState* GS = GetPFGameState();
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
			if (Cast<ACombatForgePlayerState>(PSBase))
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

void ACombatForgeGameMode::TrimOneBotFromTeam(uint8 Team)
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

void ACombatForgeGameMode::RemoveAllBots()
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	// Collect first — destroying a controller mutates PlayerArray under the iterator.
	TArray<APFBotController*> Bots;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

void ACombatForgeGameMode::BeginLiveRound()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;
	}

	// Fresh ammo stations each Live (4 random field spots).
	SpawnAmmoBarrels();

	if (GS->MatchType == EPFMatchType::Skirmish)
	{
		// One continuous Live period; the match ends by tag-cap (NotifyPawnEliminated) or the timer.
		// NO CheckElimVictory — Skirmish never resolves by team-wipe (everyone respawns).
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&ACombatForgeGameMode::ResolveSkirmishOnTimer, SkirmishMatchDuration, false);
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: Skirmish LIVE (%.0f s, first to %d tags)"),
			SkirmishMatchDuration, SkirmishTagTarget);
		return;
	}

	if (GS->MatchType == EPFMatchType::FreeForAll)
	{
		// Solo continuous Live period — same clock as Skirmish, per-player TagCount win.
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&ACombatForgeGameMode::ResolveFreeForAllOnTimer, SkirmishMatchDuration, false);
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: FreeForAll LIVE (%.0f s, first to %d tags)"),
			SkirmishMatchDuration, SkirmishTagTarget);
		return;
	}

	if (GS->MatchType == EPFMatchType::CaptureFlag)
	{
		SpawnObjectiveActors();
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&ACombatForgeGameMode::ResolveCaptureFlagOnTimer, SkirmishMatchDuration, false);
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: CaptureFlag LIVE (%.0f s, first to %d captures)"),
			SkirmishMatchDuration, CaptureFlagTarget);
		return;
	}

	if (GS->MatchType == EPFMatchType::Domination)
	{
		// CoD-style Domination (playtest + research rework): ALL THREE zones live simultaneously, no rotation.
		// A zone is captured by standing in it uncontested — 15s solo for a neutral zone, faster with teammates
		// (x min(N,3)), 2x total for an enemy zone (neutralize, then capture). Progress persists when the zone
		// empties (Tom's spec), freezes while contested (CoD). Income: 1 pt per owned zone per 5s, first to 200.
		DominationIncomeTickCounter = 0;
		SpawnObjectiveActors();
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&ACombatForgeGameMode::ResolveDominationOnTimer, SkirmishMatchDuration, false);
		GetWorldTimerManager().SetTimer(ObjectiveScoreTimerHandle, this,
			&ACombatForgeGameMode::TickDominationScoring, ObjectiveScoreInterval, true);
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: Domination LIVE (%.0f s, capture %.0f s, first to %d, all zones active)"),
			SkirmishMatchDuration, DominationCaptureSeconds, DominationTargetScore);
		return;
	}

	if (GS->MatchType == EPFMatchType::Hardpoint)
	{
		HardpointActiveSlot = 0;
		SpawnObjectiveActors();
		GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + SkirmishMatchDuration);
		ApplyServerMoveLocks();
		GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
			&ACombatForgeGameMode::ResolveHardpointOnTimer, SkirmishMatchDuration, false);
		GetWorldTimerManager().SetTimer(ObjectiveScoreTimerHandle, this,
			&ACombatForgeGameMode::TickHardpointScoring, ObjectiveScoreInterval, true);
		GetWorldTimerManager().SetTimer(HardpointRotateTimerHandle, this,
			&ACombatForgeGameMode::RotateHardpoint, HardpointRotateInterval, true);
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: Hardpoint LIVE (%.0f s, first to %d, rotate %.0f s)"),
			SkirmishMatchDuration, SkirmishTagTarget, HardpointRotateInterval);
		return;
	}

	const float Duration = bSuddenDeathRoundActive ? SuddenDeathDuration : EffectiveRoundDuration;
	GS->ServerSetRoundState(EPFRoundState::Live, GS->GetServerWorldTimeSeconds() + Duration);
	ApplyServerMoveLocks();
	GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
		&ACombatForgeGameMode::ResolveRoundOnTimer, Duration, false);
	// Breakout horn: clients play UPFCombatAudio::PlayBreakout from the RoundState delegate (T6).

	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: round %d LIVE (%.0f s)"), GS->RoundNumber, Duration);

	// A team can already be empty here (whole team disconnected during Freeze/Intermission).
	// CheckElimVictory handles 0-vs-N and 0-vs-0, so the round resolves instead of running 90 s.
	CheckElimVictory();
}

void ACombatForgeGameMode::ResolveRoundOnTimer()
{
	ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::ResolveSkirmishOnTimer()
{
	ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::EndSkirmish(uint8 WinnerTeam)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;   // guards the tag-cap + timer double-fire race
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	PendingMatchWinner = WinnerTeam;
	PendingMatchResult = MakeMatchResult(WinnerTeam);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: Skirmish over — winner team %d (%d-%d)"),
		WinnerTeam, GS->TeamScores[0], GS->TeamScores[1]);
	SetPhase(EPFMatchPhase::Vote);   // same Combat→Vote jump EndRound uses for a decided match
}

void ACombatForgeGameMode::CheckSkirmishAbandon()
{
	// Disconnect path: Skirmish never resolves by team-wipe (tagged players respawn), but a whole team
	// LEAVING has no respawn — end promptly instead of running the full clock with no opponents. In
	// Skirmish, bAliveInRound is never cleared by a tag, so AliveCounts == connected teamed players.
	const ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::ResolveFreeForAllOnTimer()
{
	ACombatForgeGameState* GS = GetPFGameState();
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
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

void ACombatForgeGameMode::EndFreeForAll(uint8 WinnerRosterOrNone)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat)
	{
		return;   // tag-cap + timer double-fire guard
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);
	PendingMatchWinner = WinnerRosterOrNone;   // roster index, or 255 draw (not a team id)
	PendingMatchResult = MakeMatchResult(WinnerRosterOrNone);
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: FreeForAll over — winner roster %d"),
		WinnerRosterOrNone);
	SetPhase(EPFMatchPhase::Vote);
}

void ACombatForgeGameMode::CheckFreeForAllAbandon()
{
	// If only one combatant remains connected, they win (others left / never joined).
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::FreeForAll
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	ACombatForgePlayerState* Sole = nullptr;
	int32 Count = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

bool ACombatForgeGameMode::IsTeamScoreObjectiveMode(EPFMatchType Type) const
{
	return Type == EPFMatchType::CaptureFlag
		|| Type == EPFMatchType::Domination
		|| Type == EPFMatchType::Hardpoint;
}

void ACombatForgeGameMode::EndTeamScoreObjective(uint8 WinnerTeam, const TCHAR* ModeName)
{
	ACombatForgeGameState* GS = GetPFGameState();
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
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: %s over — winner team %d (%d-%d)"),
		ModeName, WinnerTeam, GS->TeamScores[0], GS->TeamScores[1]);
	SetPhase(EPFMatchPhase::Vote);
}

void ACombatForgeGameMode::ResolveCaptureFlagOnTimer()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1]) { Winner = 0; }
	else if (GS->TeamScores[1] > GS->TeamScores[0]) { Winner = 1; }
	EndCaptureFlag(Winner);
}

void ACombatForgeGameMode::EndCaptureFlag(uint8 WinnerTeam)
{
	EndTeamScoreObjective(WinnerTeam, TEXT("CaptureFlag"));
}

void ACombatForgeGameMode::CheckCaptureFlagAbandon()
{
	const ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::ResolveDominationOnTimer()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1]) { Winner = 0; }
	else if (GS->TeamScores[1] > GS->TeamScores[0]) { Winner = 1; }
	EndDomination(Winner);
}

void ACombatForgeGameMode::EndDomination(uint8 WinnerTeam)
{
	EndTeamScoreObjective(WinnerTeam, TEXT("Domination"));
}

void ACombatForgeGameMode::CheckDominationAbandon()
{
	const ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::ResolveHardpointOnTimer()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	uint8 Winner = TeamNone;
	if (GS->TeamScores[0] > GS->TeamScores[1]) { Winner = 0; }
	else if (GS->TeamScores[1] > GS->TeamScores[0]) { Winner = 1; }
	EndHardpoint(Winner);
}

void ACombatForgeGameMode::EndHardpoint(uint8 WinnerTeam)
{
	EndTeamScoreObjective(WinnerTeam, TEXT("Hardpoint"));
}

void ACombatForgeGameMode::CheckHardpointAbandon()
{
	const ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::SpawnObjectiveActors()
{
	UWorld* World = GetWorld();
	ACombatForgeGameState* GS = GetPFGameState();
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
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: CTF flags spawned at PFGrid homes"));
		return;
	}

	if (GS->MatchType == EPFMatchType::Domination || GS->MatchType == EPFMatchType::Hardpoint)
	{
		const bool bHardpoint = (GS->MatchType == EPFMatchType::Hardpoint);
		ControlPoints.Reset(PFObjectiveLayout::ControlPointCount);
		for (int32 i = 0; i < PFObjectiveLayout::ControlPointCount; ++i)
		{
			const FVector Loc = PFObjectiveLayout::ControlPointLocation(i);
			// Hardpoint: one active hill that rotates. Domination (CoD model): ALL zones live at once.
			const bool bActive = bHardpoint ? (i == HardpointActiveSlot) : true;
			APFControlPointActor* CP = World->SpawnActor<APFControlPointActor>(
				APFControlPointActor::StaticClass(), Loc, FRotator::ZeroRotator, Params);
			if (CP)
			{
				CP->ServerInit(i, Loc, bActive);
				ControlPoints.Add(CP);
			}
		}
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: %d control points spawned (%s)"),
			ControlPoints.Num(), bHardpoint ? TEXT("Hardpoint") : TEXT("Domination"));
	}
}

void ACombatForgeGameMode::DestroyObjectiveActors()
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

void ACombatForgeGameMode::SpawnAmmoBarrels()
{
	if (!HasAuthority() || !ArenaShell)
	{
		return;
	}
	DestroyAmmoBarrels();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;

	// Four well-separated random field spots (re-roll if too close).
	TArray<FVector> Spots;
	const int32 Wanted = 4;
	for (int32 Attempt = 0; Attempt < 40 && Spots.Num() < Wanted; ++Attempt)
	{
		const FTransform T = ArenaShell->GetRandomFieldSpawnTransform(
			0xBEEF + Attempt * 17 + Spots.Num() * 91
			+ static_cast<int32>(World->GetTimeSeconds() * 10.f));
		FVector Loc = T.GetLocation();
		Loc.Z = 0.f;   // barrels sit on the Z=0 floor (mesh is grounded relative to the actor root); the
		               // spawn transform's SpawnZ=100 is for pawns and would float the barrel.
		bool bFar = true;
		for (const FVector& Existing : Spots)
		{
			if (FVector::DistSquared(Existing, Loc) < FMath::Square(900.f))
			{
				bFar = false;
				break;
			}
		}
		if (bFar)
		{
			Spots.Add(Loc);
		}
	}
	// Fallback fill if random clustering failed. Quarter-points of the LIVE field (The Yard is
	// 6400×8000 — the old hard-coded 6400×4000 would cluster all four barrels in its south half).
	while (Spots.Num() < Wanted)
	{
		const FVector2D Field = ArenaShell->GetFieldSize();
		const float Fx = Field.X, Fy = Field.Y;
		const FVector Corners[4] = {
			FVector(Fx * 0.25f, Fy * 0.25f, 0.f),
			FVector(Fx * 0.75f, Fy * 0.25f, 0.f),
			FVector(Fx * 0.25f, Fy * 0.75f, 0.f),
			FVector(Fx * 0.75f, Fy * 0.75f, 0.f),
		};
		Spots.Add(Corners[Spots.Num() % 4]);
	}

	for (int32 i = 0; i < Wanted; ++i)
	{
		APFAmmoBarrel* Barrel = World->SpawnActor<APFAmmoBarrel>(
			APFAmmoBarrel::StaticClass(), Spots[i], FRotator::ZeroRotator, Params);
		if (Barrel)
		{
			Barrel->ServerActivateAt(Spots[i]);
			AmmoBarrels.Add(Barrel);
		}
	}
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: spawned %d ammo barrels"), AmmoBarrels.Num());
}

void ACombatForgeGameMode::DestroyAmmoBarrels()
{
	for (APFAmmoBarrel* B : AmmoBarrels)
	{
		if (B)
		{
			B->Destroy();
		}
	}
	AmmoBarrels.Reset();
}

void ACombatForgeGameMode::ClearAllFlagCarriers()
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
		{
			if (PS->bCarryingFlag || PS->StandingOnPoint != 255)
			{
				PS->ServerSetFlagCarry(false, 255);
				PS->ServerSetStandingOnPoint(255);
			}
		}
	}
}

APFFlagActor* ACombatForgeGameMode::GetFlagForTeam(uint8 Team) const
{
	if (Team > 1)
	{
		return nullptr;
	}
	return Flags[Team];
}

void ACombatForgeGameMode::NotifyFlagTouched(APFFlagActor* Flag, ACombatForgePlayerState* Toucher)
{
	ACombatForgeGameState* GS = GetPFGameState();
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
			UE_LOG(CombatForgeLog, Log, TEXT("GameMode: %s returned team %d flag"),
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
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: CTF capture by %s (team %d) → %d-%d"),
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
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: %s picked up team %d flag"),
		*Toucher->GetPlayerName(), FlagTeam);
}

void ACombatForgeGameMode::TickDominationScoring()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Domination
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}

	// Clear standing-on-point stamps, then re-stamp from occupancy.
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
		{
			PS->ServerSetStandingOnPoint(255);
		}
	}

	// CoD-style Domination: every zone runs its own capture chain (the math + replication live on the zone
	// actor — ServerTickCapture); this tick feeds each zone its occupancy, stamps PS for the HUD, pays the
	// owner income, and checks the win.
	int32 OwnedZones[2] = { 0, 0 };
	for (APFControlPointActor* CP : ControlPoints)
	{
		if (!CP || !CP->IsPointActive())
		{
			continue;
		}
		int32 OutA = 0, OutB = 0;
		TArray<ACombatForgePlayerState*> Occupants;
		CP->ServerQueryOccupancy(OutA, OutB, &Occupants);

		// Stamp PS from the SAME overlap set that counted them — the old separate center-distance test used
		// a smaller effective radius than the sphere overlap (capsule extent), so edge-standers captured
		// zones with no capture bar on their own screen.
		for (ACombatForgePlayerState* PS : Occupants)
		{
			PS->ServerSetStandingOnPoint(static_cast<uint8>(CP->GetPointIndex()));
		}

		if (CP->ServerTickCapture(OutA, OutB, ObjectiveScoreInterval, DominationCaptureSeconds))
		{
			UE_LOG(CombatForgeLog, Log, TEXT("Domination: zone %d -> team %d"),
				CP->GetPointIndex(), CP->GetControllingTeam());
		}
		if (CP->GetControllingTeam() <= 1)
		{
			++OwnedZones[CP->GetControllingTeam()];
		}
	}

	// Owner income: 1 pt per owned zone per 5 seconds (CoD: 1 pt/flag/5s tick, first to 200). Presence not
	// required once captured — and per BO6, contested does NOT pause an owned zone's income, only capture.
	if (++DominationIncomeTickCounter >= 5)
	{
		DominationIncomeTickCounter = 0;
		if (OwnedZones[0] > 0 || OwnedZones[1] > 0)
		{
			const uint16 ScoreA = static_cast<uint16>(GS->TeamScores[0] + OwnedZones[0]);
			const uint16 ScoreB = static_cast<uint16>(GS->TeamScores[1] + OwnedZones[1]);
			GS->ServerSetTeamScores(ScoreA, ScoreB);
			if (ScoreA >= DominationTargetScore || ScoreB >= DominationTargetScore)
			{
				// Strict compares: a single-team cross is strictly ahead; a same-tick double cross at equal
				// score is a DRAW (TeamNone), matching the timer path instead of silently crowning team 0.
				uint8 Winner = TeamNone;
				if (ScoreA > ScoreB) { Winner = 0; }
				else if (ScoreB > ScoreA) { Winner = 1; }
				EndDomination(Winner);
			}
		}
	}
}

void ACombatForgeGameMode::TickHardpointScoring()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->MatchType != EPFMatchType::Hardpoint
		|| GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}

	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
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
	TArray<ACombatForgePlayerState*> Occupants;
	const uint8 Sole = Active->ServerQueryOccupancy(OutA, OutB, &Occupants);

	// Hardpoint: pad is neutral unless a single team is standing on it right now.
	// Empty / contested → no owner, no score (sticky owner was awarding points to an empty hill).
	if (Sole <= 1)
	{
		Active->ServerSetControllingTeam(Sole);
	}
	else
	{
		Active->ServerSetControllingTeam(255);
	}

	// Stamp from the same overlap set that counted them (see TickDominationScoring).
	for (ACombatForgePlayerState* PS : Occupants)
	{
		PS->ServerSetStandingOnPoint(static_cast<uint8>(Active->GetPointIndex()));
	}

	// Score only while sole occupancy this tick.
	if (Sole > 1)
	{
		return;
	}
	uint16 ScoreA = GS->TeamScores[0];
	uint16 ScoreB = GS->TeamScores[1];
	if (Sole == 0) { ++ScoreA; }
	else { ++ScoreB; }   // Sole == 1
	GS->ServerSetTeamScores(ScoreA, ScoreB);
	if ((Sole == 0 ? ScoreA : ScoreB) >= SkirmishTagTarget)
	{
		EndHardpoint(Sole);
	}
}

void ACombatForgeGameMode::RotateHardpoint()
{
	ACombatForgeGameState* GS = GetPFGameState();
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
	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: Hardpoint rotated to slot %d"), HardpointActiveSlot);
}

void ACombatForgeGameMode::CheckElimVictory()
{
	ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::EndRound(uint8 WinnerTeam)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(RoundTimerHandle);

	// T18 scoring: survival 25 to everyone still alive, round win 50 to every winning teammate.
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: match decided (winner %d, %d-%d)"),
			PendingMatchWinner, WinsA, WinsB);
		SetPhase(EPFMatchPhase::Vote);
		return;
	}

	GS->ServerSetRoundState(EPFRoundState::Intermission, GS->GetServerWorldTimeSeconds() + IntermissionDuration);
	ApplyServerMoveLocks();
	GetWorldTimerManager().SetTimer(RoundTimerHandle, this,
		&ACombatForgeGameMode::OnIntermissionEnd, IntermissionDuration, false);

	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: round %d over (winner %d) - score %d-%d"),
		GS->RoundNumber, WinnerTeam, WinsA, WinsB);
}

void ACombatForgeGameMode::OnIntermissionEnd()
{
	ACombatForgeGameState* GS = GetPFGameState();
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

void ACombatForgeGameMode::StartSuddenDeath()
{
	// The one entry point into the sudden-death round (reached from OnIntermissionEnd after a
	// post-max-rounds deadlock flagged bPendingSuddenDeath in EndRound).
	bPendingSuddenDeath = true;
	StartNextRound();
}

FPFMatchResult ACombatForgeGameMode::MakeMatchResult(uint8 MatchWinner) const
{
	const ACombatForgeGameState* GS = GetPFGameState();
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
				if (const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
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

void ACombatForgeGameMode::NotifyPawnEliminated(ACombatForgeCharacter* Victim, const FPFPaintHitInfo& FinalHit)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || !Victim)
	{
		return;
	}
	ACombatForgePlayerState* VictimPS = Victim->GetPlayerState<ACombatForgePlayerState>();
	if (!VictimPS)
	{
		return;
	}
	ACombatForgePlayerState* ShooterPS = FinalHit.ShooterPS;

	// CONTRACT-GAP: the contract specifies self-resetting dummies (T29) but is silent on player
	// pawns eliminated in the warm-up pen (Lobby fire is live, T21). Smallest implementation:
	// stats-free 1 s reset back to the pen, mirroring the dummy behavior.
	if (GS->Phase == EPFMatchPhase::Lobby)
	{
		// Freeform warmup: near-instant, stats-free reset with a FULL loadout — die, pop back, keep playing.
		if (VictimPS)
		{
			VictimPS->ServerSetOutWaitingRespawn(GS->GetServerWorldTimeSeconds() + 0.35f);
		}
		TWeakObjectPtr<ACombatForgeCharacter> WeakVictim(Victim);
		TWeakObjectPtr<ACombatForgePlayerState> WeakVictimPS(VictimPS);
		TWeakObjectPtr<ACombatForgeGameMode> WeakThis(this);
		FTimerHandle ResetHandle;
		GetWorldTimerManager().SetTimer(ResetHandle,
			FTimerDelegate::CreateLambda([WeakThis, WeakVictim, WeakVictimPS]()
			{
				if (!WeakThis.IsValid())
				{
					return;
				}
				if (WeakVictim.IsValid())
				{
					if (ACombatForgePlayerState* PS = WeakVictim->GetPlayerState<ACombatForgePlayerState>())
					{
						PS->ServerClearOutState();
					}
					WeakVictim->GetHealth()->ResetForRound(3);
					if (UPFWeaponComponent* Weapon = WeakVictim->GetWeapon())
					{
						Weapon->ServerResetLoadout();   // warmup deaths refill everything too
					}
					if (ACombatForgePlayerState* PS = WeakVictim->GetPlayerState<ACombatForgePlayerState>())
					{
						WeakThis->TeleportPawnTo(WeakVictim.Get(), WeakThis->GetSpawnTransform(PS));
					}
				}
				else if (WeakVictimPS.IsValid())
				{
					// Pawn destroyed mid-window (field roaming = KillZ is reachable now) — restart instead of stranding.
					ACombatForgePlayerState* PS = WeakVictimPS.Get();
					PS->ServerClearOutState();
					if (AController* C = PS->GetOwningController())
					{
						WeakThis->RestartPlayer(C);
					}
				}
			}),
			0.35f, false);
		return;
	}

	if (GS->Phase != EPFMatchPhase::Combat || GS->RoundState != EPFRoundState::Live)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("GameMode: elimination outside a live round ignored (%s)"),
			*VictimPS->GetPlayerName());
		return;
	}

	// Fall death (ShooterTeam 255): no elim credit, near-instant respawn in every mode.
	const bool bFallDeath = (ShooterPS == nullptr && FinalHit.ShooterTeam == 255);
	if (bFallDeath)
	{
		VictimPS->TimesEliminated = VictimPS->TimesEliminated + 1;
		FPFElimEntry Entry;
		Entry.ShooterName = TEXT("Fall");
		Entry.ShooterTeam = 255;
		Entry.VictimName = VictimPS->GetPlayerName();
		Entry.VictimTeam = VictimPS->TeamId;
		Entry.ServerTime = GS->GetServerWorldTimeSeconds();
		GS->ServerAddElimEntry(Entry);
		// Drop flag if carrying.
		if (VictimPS->bCarryingFlag)
		{
			if (APFFlagActor* Carried = GetFlagForTeam(VictimPS->CarriedFlagTeam))
			{
				Carried->ServerDropAt(Victim->GetActorLocation());
			}
			VictimPS->ServerSetFlagCarry(false, 255);
		}
		VictimPS->ServerSetStandingOnPoint(255);
		UE_LOG(CombatForgeLog, Log, TEXT("GameMode: fall death → instant respawn (%s, fall≥3 levels)"),
			*VictimPS->GetPlayerName());
		RespawnVictimAtTeamSpawn(Victim, 0.35f);   // near-instant (short "you're out" flash)
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
	VictimPS->ServerSetOutForRound();   // HUD: "YOU'RE OUT" / out for this round
	if (ACombatForgePlayerController* VictimPC = Cast<ACombatForgePlayerController>(VictimPS->GetPlayerController()))
	{
		VictimPC->SetEliminatedMoveLock(true);
		VictimPC->StartDeathCamera();
	}
	RecountAlive();

	// Already-dead spectators whose view target was the victim's now-hidden pawn move on to
	// another living teammate (or back to their own body when none remain).
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(It->Get()))
		{
			PC->RetargetSpectatorFrom(Victim);
		}
	}

	CheckElimVictory();
}

void ACombatForgeGameMode::RecountAlive()
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	uint8 Alive[2] = {0, 0};
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

void ACombatForgeGameMode::SubmitVote(ACombatForgePlayerController* Voter, EPFThumbVote Thumb,
	const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Vote || !Voter)
	{
		return;
	}
	ACombatForgePlayerState* PS = Voter->GetPlayerState<ACombatForgePlayerState>();
	if (!PS || PS->bHasVoted)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("GameMode: duplicate/invalid vote rejected"));
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

void ACombatForgeGameMode::CheckAllVotesIn(const ACombatForgePlayerState* IgnorePS)
{
	// T3 early-advance, re-evaluated on every vote AND on disconnect (the last non-voter
	// leaving completes the condition; their lingering PlayerState is excluded). No abstain
	// fill needed here — everyone counted has voted, so FinalizeVotePhase has nothing to add.
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || GS->Phase != EPFMatchPhase::Vote)
	{
		return;
	}
	for (APlayerState* OtherBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* Other = Cast<ACombatForgePlayerState>(OtherBase);
		if (Other && Other != IgnorePS && !Other->IsABot() && !Other->bHasVoted)
		{
			return;   // bots don't vote — they don't hold the vote phase open
		}
	}
	SetPhase(EPFMatchPhase::Results);
}

void ACombatForgeGameMode::FinalizeVotePhase()
{
	ACombatForgeGameState* GS = GetPFGameState();
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
		ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

void ACombatForgeGameMode::SanitizeVoteIds(TArray<uint8>& InOutLiked, TArray<uint8>& InOutDisliked)
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

void ACombatForgeGameMode::ApplyServerMoveLocks()
{
	// §4.5: move input ignored in Freeze / Intermission / Vote / Results; look stays free.
	const ACombatForgeGameState* GS = GetPFGameState();
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
		if (ACombatForgePlayerController* PC = Cast<ACombatForgePlayerController>(It->Get()))
		{
			PC->ApplyServerMoveLock(bLocked);
		}
	}
}

void ACombatForgeGameMode::ResetPlayerMatchStats()
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase))
		{
			PS->Eliminations = 0;
			PS->TimesEliminated = 0;
			PS->TagCount = 0;
			PS->MatchScore = 0;
			PS->bHasVoted = false;
			PS->ServerSetFlagCarry(false, 255);
			PS->ServerSetStandingOnPoint(255);
			PS->ServerClearOutState();
			PS->ForceNetUpdate();
		}
	}
}

ACombatForgeGameState* ACombatForgeGameMode::GetPFGameState() const
{
	return GetGameState<ACombatForgeGameState>();
}

UPFRatingSubsystem* ACombatForgeGameMode::GetRatingSubsystem() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UPFRatingSubsystem>() : nullptr;
}

bool ACombatForgeGameMode::IsActiveRosterMember(const ACombatForgePlayerState* PS)
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

void ACombatForgeGameMode::LogCrashBreadcrumb()
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	int32 Humans = 0, Bots = 0, Ghosts = 0;
	FString Names;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
		if (!PS)
		{
			continue;
		}
		if (!IsActiveRosterMember(PS))
		{
			++Ghosts;
			continue;
		}
		if (PS->IsABot()) { ++Bots; }
		else
		{
			++Humans;
			if (!Names.IsEmpty()) { Names += TEXT(","); }
			Names += PS->GetPlayerName();
		}
	}
	UE_LOG(CombatForgeLog, Log,
		TEXT("CRASH_BC phase=%d round=%d type=%d humans=%d bots=%d ghosts=%d scores=%d-%d alive=%d-%d [%s]"),
		static_cast<int32>(GS->Phase), GS->RoundNumber, static_cast<int32>(GS->MatchType),
		Humans, Bots, Ghosts,
		GS->TeamScores[0], GS->TeamScores[1],
		GS->AliveCounts[0], GS->AliveCounts[1],
		*Names);
}

void ACombatForgeGameMode::ScrubGhostPlayerStates(const ACombatForgePlayerState* KeepPS)
{
	ACombatForgeGameState* GS = GetPFGameState();
	if (!GS || !HasAuthority())
	{
		return;
	}
	TArray<ACombatForgePlayerState*> ToKill;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
		if (!PS || PS == KeepPS || PS->IsABot())
		{
			continue;
		}
		if (!IsActiveRosterMember(PS))
		{
			ToKill.Add(PS);
		}
	}
	for (ACombatForgePlayerState* Ghost : ToKill)
	{
		UE_LOG(CombatForgeLog, Warning, TEXT("GameMode: scrubbing ghost PlayerState %s"),
			*Ghost->GetPlayerName());
		if (TObjectPtr<APFTargetDummy>* Dummy = WarmupDummies.Find(Ghost))
		{
			if (*Dummy) { (*Dummy)->Destroy(); }
			WarmupDummies.Remove(Ghost);
		}
		Ghost->Destroy();
	}
}

void ACombatForgeGameMode::GetTeamCounts(int32& OutTeamA, int32& OutTeamB) const
{
	OutTeamA = 0;
	OutTeamB = 0;
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

bool ACombatForgeGameMode::AreAllPlayersReady(const ACombatForgePlayerState* IgnorePS) const
{
	const ACombatForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		return false;
	}
	int32 Counted = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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

uint8 ACombatForgeGameMode::FindFreeRosterIndex() const
{
	const ACombatForgeGameState* GS = GetPFGameState();
	for (uint8 Candidate = 0; Candidate < PFGrid::MaxRosterSlots; ++Candidate)
	{
		bool bTaken = false;
		if (GS)
		{
			for (APlayerState* PSBase : GS->PlayerArray)
			{
				const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
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
	UE_LOG(CombatForgeLog, Error, TEXT("GameMode: roster full (12) - reusing slot 11"));
	return PFGrid::MaxRosterSlots - 1;
}

void ACombatForgeGameMode::ComputeEffectiveScaling()
{
	// Continuous timed modes: bypass Elimination round scaling. RoundWinsToTake carries the score
	// TARGET for the HUD (CTF uses CaptureFlagTarget; others use SkirmishTagTarget).
	if (ACombatForgeGameState* SkGS = GetPFGameState())
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
			UE_LOG(CombatForgeLog, Log, TEXT("GameMode: match type %d — first to %d, %.0f s"),
				static_cast<int32>(SkGS->MatchType), Target, SkirmishMatchDuration);
			return;
		}
	}

	// Format = the SELECTED team size (bots fill to it), not the live human count — so a 2-human 4v4
	// still plays first-to-4, not the ≤2v2 small format. With bots off, fall back to live team counts.
	const ACombatForgeGameState* GS = GetPFGameState();
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
	if (ACombatForgeGameState* MutableGS = GetPFGameState())
	{
		MutableGS->ServerSetRoundWinsToTake(EffectiveRoundWinsToTake);
	}

	UE_LOG(CombatForgeLog, Log, TEXT("GameMode: match format first-to-%d, max %d rounds, %.0f s rounds"),
		EffectiveRoundWinsToTake, EffectiveMaxRounds, EffectiveRoundDuration);
}
