// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PaintForgePlayerController.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameInstance.h"
#include "Core/PaintForgeGameMode.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Player/PaintForgeCharacter.h"
#include "Input/PFInputConfig.h"
#include "UI/PFRootHUDWidget.h"
#include "Combat/PFCombatAudio.h"

#include "Blueprint/UserWidget.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "TimerManager.h"

namespace
{
	constexpr float InputRetryInterval = 0.25f;   // 02 R1: subsystem may not exist at first call
	constexpr float DeathCamDuration   = 0.5f;    // T5: locked death cam before teammate spectate
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void APaintForgePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// All input objects are native NewObjects rooted in UPFInputConfig (02 R1). Built exactly
	// once; phase changes only Add/Remove mapping contexts, never rebuild objects.
	if (!InputConfig)
	{
		InputConfig = NewObject<UPFInputConfig>(this, TEXT("PFInputConfig"));
		InputConfig->Build(this);
	}

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC || !InputConfig)
	{
		UE_LOG(PaintForgeLog, Error, TEXT("PC: EnhancedInputComponent missing - PC-level actions unbound"));
		return;
	}

	// PC-level actions (§3.2): ready, scoreboard, menu-back, host start — plus the dead-only
	// Fire/ADS spectate cycle (T5).
	EIC->BindAction(InputConfig->IA_Ready,      ETriggerEvent::Started,   this, &APaintForgePlayerController::OnReadyToggle);
	EIC->BindAction(InputConfig->IA_HostStart,  ETriggerEvent::Started,   this, &APaintForgePlayerController::OnHostStartPressed);
	EIC->BindAction(InputConfig->IA_Scoreboard, ETriggerEvent::Started,   this, &APaintForgePlayerController::OnScoreboardStarted);
	EIC->BindAction(InputConfig->IA_Scoreboard, ETriggerEvent::Completed, this, &APaintForgePlayerController::OnScoreboardCompleted);
	EIC->BindAction(InputConfig->IA_MenuBack,   ETriggerEvent::Started,   this, &APaintForgePlayerController::OnMenuBack);
	EIC->BindAction(InputConfig->IA_Fire,       ETriggerEvent::Started,   this, &APaintForgePlayerController::OnFireWhileDead);
	EIC->BindAction(InputConfig->IA_ADS,        ETriggerEvent::Started,   this, &APaintForgePlayerController::OnADSWhileDead);
}

UPFInputConfig* APaintForgePlayerController::GetInputConfig() const
{
	return InputConfig;
}

void APaintForgePlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (IsLocalController())
	{
		TryBindGameState();
	}
}

void APaintForgePlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();
	if (IsLocalController())
	{
		CreateHUDIfNeeded();
		ApplyInputForPhase();   // internally retries until the EI subsystem is alive (02 R1)
		TrySendGuidHash();
	}
}

void APaintForgePlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	if (IsLocalController())
	{
		ApplyInputForPhase();
	}
}

void APaintForgePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (APaintForgeGameState* GS = GetPFGameState())
	{
		GS->OnPhaseChangedEvent.RemoveAll(this);
		GS->OnRoundStateChangedEvent.RemoveAll(this);
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(InputRetryHandle);
		World->GetTimerManager().ClearTimer(GameStateRetryHandle);
		World->GetTimerManager().ClearTimer(DeathCamHandle);
	}
	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// GameState binding + phase reaction (local controllers only)
// ---------------------------------------------------------------------------

APaintForgeGameState* APaintForgePlayerController::GetPFGameState() const
{
	UWorld* World = GetWorld();
	return World ? World->GetGameState<APaintForgeGameState>() : nullptr;
}

void APaintForgePlayerController::TryBindGameState()
{
	if (bGameStateBound)
	{
		return;
	}
	APaintForgeGameState* GS = GetPFGameState();
	if (!GS)
	{
		// GameState may replicate in after the PC on clients — retry until it exists.
		GetWorldTimerManager().SetTimer(GameStateRetryHandle, this,
			&APaintForgePlayerController::TryBindGameState, InputRetryInterval, false);
		return;
	}
	GS->OnPhaseChangedEvent.AddUObject(this, &APaintForgePlayerController::HandlePhaseChanged);
	GS->OnRoundStateChangedEvent.AddUObject(this, &APaintForgePlayerController::HandleRoundStateChanged);
	bGameStateBound = true;

	// Catch up: join-in-progress must react to the state that already replicated (02 R4).
	HandlePhaseChanged(GS->Phase);
	HandleRoundStateChanged(GS->RoundState);
}

void APaintForgePlayerController::HandlePhaseChanged(EPFMatchPhase NewPhase)
{
	if (!IsLocalController())
	{
		return;
	}
	ApplyInputForPhase();
}

void APaintForgePlayerController::HandleRoundStateChanged(EPFRoundState NewState)
{
	if (!IsLocalController())
	{
		return;
	}
	ApplyInputForPhase();

	// Breakout horn stub at freeze → live (T6); local pawn only.
	if (NewState == EPFRoundState::Live)
	{
		if (APaintForgeCharacter* PFPawn = Cast<APaintForgeCharacter>(GetPawn()))
		{
			if (UPFCombatAudio* Audio = PFPawn->GetCombatAudio())
			{
				Audio->PlayBreakout();
			}
		}
	}
}

void APaintForgePlayerController::ApplyInputForPhase()
{
	if (!IsLocalController())
	{
		return;
	}

	const APaintForgeGameState* GS = GetPFGameState();
	ULocalPlayer* LP = Cast<ULocalPlayer>(Player);
	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;

	if (!GS || !Subsystem || !InputConfig ||
		!InputConfig->IMC_Common || !InputConfig->IMC_Combat || !InputConfig->IMC_Build)
	{
		// 02 R1: the subsystem may not be alive yet on clients — retry, never crash, never skip.
		GetWorldTimerManager().SetTimer(InputRetryHandle, this,
			&APaintForgePlayerController::ApplyInputForPhase, InputRetryInterval, false);
		return;
	}
	GetWorldTimerManager().ClearTimer(InputRetryHandle);

	// §4.5 phase → IMC table.
	bool bWantCommon = false, bWantCombat = false, bWantBuild = false;
	bool bUIOnly = false;
	bool bLocalMoveLock = false;

	switch (GS->Phase)
	{
	case EPFMatchPhase::Lobby:
		bWantCommon = true;
		bWantCombat = true;   // warm-up pen fire live (T21)
		break;
	case EPFMatchPhase::Build:
		bWantCommon = true;
		bWantBuild = true;    // build always-on, weapons dead (T4)
		break;
	case EPFMatchPhase::Combat:
		bWantCommon = true;
		switch (GS->RoundState)
		{
		case EPFRoundState::Freeze:
			bWantCombat = true;         // look/ADS free; move ignored; fire server-rejected
			bLocalMoveLock = true;
			break;
		case EPFRoundState::Live:
			bWantCombat = true;
			break;
		case EPFRoundState::Intermission:
			bLocalMoveLock = true;      // Common only (score strip)
			break;
		default:
			bWantCombat = true;
			break;
		}
		break;
	case EPFMatchPhase::Vote:
	case EPFMatchPhase::Results:
		bUIOnly = true;
		bLocalMoveLock = true;
		break;
	}

	auto SyncContext = [Subsystem](UInputMappingContext* IMC, bool bWanted, int32 Priority)
	{
		const bool bApplied = Subsystem->HasMappingContext(IMC);
		if (bWanted && !bApplied)
		{
			Subsystem->AddMappingContext(IMC, Priority);
		}
		else if (!bWanted && bApplied)
		{
			Subsystem->RemoveMappingContext(IMC);
		}
	};
	SyncContext(InputConfig->IMC_Common, bWantCommon, 0);
	SyncContext(InputConfig->IMC_Combat, bWantCombat, 1);
	SyncContext(InputConfig->IMC_Build,  bWantBuild,  1);

	// Input mode + cursor (§4.5). Lobby Tab-hold shows the roster with a cursor (T22).
	if (bUIOnly)
	{
		SetInputMode(FInputModeUIOnly());
		SetShowMouseCursor(true);
	}
	else if (GS->Phase == EPFMatchPhase::Lobby && bScoreboardHeld)
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		SetShowMouseCursor(true);
	}
	else
	{
		SetInputMode(FInputModeGameOnly());
		SetShowMouseCursor(false);
	}

	ApplyLocalMoveLock(bLocalMoveLock);
}

void APaintForgePlayerController::ApplyLocalMoveLock(bool bLocked)
{
	bPhaseMoveLock = bLocked;
	RefreshMoveLock();
}

void APaintForgePlayerController::RefreshMoveLock()
{
	// SetIgnoreMoveInput stacks a refcount — reset first so repeated phase reactions stay
	// idempotent. Look input is never ignored (look stays free in Freeze, T6). The elimination
	// lock survives phase-driven re-applies because both flags compose here.
	ResetIgnoreMoveInput();
	if (bPhaseMoveLock || bEliminatedMoveLock)
	{
		SetIgnoreMoveInput(true);
	}
}

void APaintForgePlayerController::ApplyServerMoveLock(bool bLocked)
{
	if (!HasAuthority())
	{
		return;
	}
	ApplyLocalMoveLock(bLocked);
}

void APaintForgePlayerController::SetEliminatedMoveLock(bool bLocked)
{
	if (!HasAuthority())
	{
		return;
	}
	// Server copy (authoritative move-ignore) + owning-client mirror: IgnoreMoveInput does not
	// replicate, so without the RPC a remote client's dead pawn keeps accepting move input.
	if (bEliminatedMoveLock != bLocked)
	{
		bEliminatedMoveLock = bLocked;
		RefreshMoveLock();
	}
	ClientSetEliminatedMoveLock(bLocked);
}

void APaintForgePlayerController::ClientSetEliminatedMoveLock_Implementation(bool bLocked)
{
	bEliminatedMoveLock = bLocked;
	RefreshMoveLock();
}

void APaintForgePlayerController::CreateHUDIfNeeded()
{
	if (RootHUD || !IsLocalController())
	{
		return;
	}
	RootHUD = CreateWidget<UPFRootHUDWidget>(this, UPFRootHUDWidget::StaticClass());
	if (RootHUD)
	{
		RootHUD->AddToViewport();
	}
	else
	{
		UE_LOG(PaintForgeLog, Error, TEXT("PC: failed to create UPFRootHUDWidget"));
	}
}

void APaintForgePlayerController::TrySendGuidHash()
{
	if (bGuidHashSent || !IsLocalController())
	{
		return;
	}
	const UPaintForgeGameInstance* GI = GetGameInstance<UPaintForgeGameInstance>();
	if (!GI)
	{
		return;
	}
	const FString Hash = GI->GetLocalPlayerGuidHash();
	if (!Hash.IsEmpty())
	{
		ServerSetPlayerGuidHash(Hash);
		bGuidHashSent = true;
	}
}

// ---------------------------------------------------------------------------
// Local input handlers
// ---------------------------------------------------------------------------

void APaintForgePlayerController::OnReadyToggle()
{
	const APaintForgeGameState* GS = GetPFGameState();
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	if (!GS || !PS)
	{
		return;
	}
	if (GS->Phase == EPFMatchPhase::Lobby || GS->Phase == EPFMatchPhase::Build)
	{
		ServerSetReady(!PS->bReady);
	}
}

void APaintForgePlayerController::OnHostStartPressed()
{
	// Client-side this is just UX; the server RPC re-validates host identity (§5 R13).
	ServerHostForceStart();
}

void APaintForgePlayerController::OnScoreboardStarted()
{
	bScoreboardHeld = true;
	OnScoreboardHeldChanged.Broadcast(true);
	ApplyInputForPhase();   // Lobby: Tab-hold cursor for host click-cycle rows (T22)
}

void APaintForgePlayerController::OnScoreboardCompleted()
{
	bScoreboardHeld = false;
	OnScoreboardHeldChanged.Broadcast(false);
	ApplyInputForPhase();
}

void APaintForgePlayerController::OnMenuBack()
{
	// v1: no pause menu; Escape is reserved. UI panels handle their own back/cancel.
	UE_LOG(PaintForgeLog, Verbose, TEXT("PC: menu back pressed"));
}

void APaintForgePlayerController::OnFireWhileDead()
{
	const APaintForgeGameState* GS = GetPFGameState();
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	if (GS && PS && GS->Phase == EPFMatchPhase::Combat && !PS->bAliveInRound)
	{
		ServerSpectateNext(true);
	}
}

void APaintForgePlayerController::OnADSWhileDead()
{
	const APaintForgeGameState* GS = GetPFGameState();
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	if (GS && PS && GS->Phase == EPFMatchPhase::Combat && !PS->bAliveInRound)
	{
		ServerSpectateNext(false);
	}
}

// ---------------------------------------------------------------------------
// Server RPCs — client-side checks are UX, these re-validate everything (§5 R13)
// ---------------------------------------------------------------------------

bool APaintForgePlayerController::IsHostController() const
{
	// Listen host = the one locally-controlled PC that also has authority (02 D12: no OSS).
	return HasAuthority() && IsLocalPlayerController();
}

void APaintForgePlayerController::ServerSetReady_Implementation(bool bNewReady)
{
	APaintForgeGameState* GS = GetPFGameState();
	APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr;
	if (!GS || !PS || !GM)
	{
		return;
	}
	if (GS->Phase != EPFMatchPhase::Lobby && GS->Phase != EPFMatchPhase::Build)
	{
		return;
	}
	PS->ServerSetReady(bNewReady);
	GM->NotifyReadyChanged();
}

void APaintForgePlayerController::ServerSubmitVote_Implementation(EPFThumbVote Thumb,
	const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds)
{
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->SubmitVote(this, Thumb, LikedIds, DislikedIds);
	}
}

void APaintForgePlayerController::ServerSetPlayerGuidHash_Implementation(const FString& GuidHash)
{
	APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	if (!PS || !PS->PlayerGuidHash.IsEmpty())
	{
		return;   // once, on join (T24)
	}
	// Expect a 40-char SHA1 hex string; sanitize defensively before trusting it to disk.
	FString Clean = GuidHash.ToLower().Left(40);
	for (const TCHAR C : Clean)
	{
		if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'))))
		{
			UE_LOG(PaintForgeLog, Warning, TEXT("PC: rejected malformed player guid hash"));
			return;
		}
	}
	if (Clean.Len() == 40)
	{
		PS->PlayerGuidHash = Clean;
		PS->ForceNetUpdate();
	}
}

void APaintForgePlayerController::ServerHostForceStart_Implementation()
{
	if (!IsHostController())
	{
		return;   // ignored if not host
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostForceStart();
	}
}

void APaintForgePlayerController::ServerHostCycleTeam_Implementation(APaintForgePlayerState* Target)
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostCycleTeam(Target);
	}
}

void APaintForgePlayerController::ServerHostReturnToLobby_Implementation()
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostReturnToLobby();
	}
}

void APaintForgePlayerController::ServerHostSetFormat_Implementation(uint8 TeamSize)
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostSetFormat(TeamSize);
	}
}

void APaintForgePlayerController::ServerHostSetBuildMode_Implementation(uint8 Mode)
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostSetBuildMode(static_cast<EPFBuildMode>(Mode));
	}
}

void APaintForgePlayerController::ServerHostSetMatchType_Implementation(uint8 Type)
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostSetMatchType(static_cast<EPFMatchType>(Type));
	}
}

void APaintForgePlayerController::PFFormat(int32 TeamSize)
{
	// Console exec on the host's controller → server RPC. e.g. "PFFormat 6" for 6v6 in the lobby.
	ServerHostSetFormat(static_cast<uint8>(FMath::Clamp(TeamSize, 1, 6)));
}

void APaintForgePlayerController::PFMode(int32 Mode)
{
	// "PFMode 0=Creative, 1=Improvement, 2=Play-only" — host, in the lobby.
	ServerHostSetBuildMode(static_cast<uint8>(FMath::Clamp(Mode, 0, 2)));
}

void APaintForgePlayerController::PFType(int32 Type)
{
	// "PFType 0=Elimination, 1=FFA, 2=Skirmish, 3=CTF, 4=Domination, 5=Hardpoint" — host, in the lobby.
	ServerHostSetMatchType(static_cast<uint8>(FMath::Clamp(Type, 0, 5)));
}

// ---------------------------------------------------------------------------
// Death cam + spectate (T5)
// ---------------------------------------------------------------------------

void APaintForgePlayerController::StartDeathCamera()
{
	if (!HasAuthority())
	{
		return;
	}
	// 0.5 s locked at the body (view target stays the hidden pawn's camera), then first-person
	// spectate of the nearest living teammate. No free-cam (info exploit, T5).
	if (APawn* MyPawn = GetPawn())
	{
		SetViewTargetWithBlend(MyPawn, 0.f);
	}
	GetWorldTimerManager().SetTimer(DeathCamHandle, this,
		&APaintForgePlayerController::SpectateNearestTeammate, DeathCamDuration, false);
}

void APaintForgePlayerController::SpectateNearestTeammate()
{
	if (!HasAuthority())
	{
		return;
	}
	TArray<APaintForgeCharacter*> Teammates;
	GatherLivingTeammatePawns(Teammates);
	if (Teammates.Num() == 0)
	{
		return;   // last one standing on the team: stay at own body until the round resolves
	}

	const FVector Here = GetPawn() ? GetPawn()->GetActorLocation() : GetFocalLocation();
	int32 BestIdx = 0;
	float BestDistSq = TNumericLimits<float>::Max();
	for (int32 Idx = 0; Idx < Teammates.Num(); ++Idx)
	{
		const float DistSq = FVector::DistSquared(Here, Teammates[Idx]->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestIdx = Idx;
		}
	}
	SpectateIndex = BestIdx;
	SetViewTargetWithBlend(Teammates[BestIdx], 0.25f);
}

void APaintForgePlayerController::RetargetSpectatorFrom(APawn* EliminatedPawn)
{
	if (!HasAuthority() || !EliminatedPawn || GetPawn() == EliminatedPawn)
	{
		return;   // the victim runs its own death-cam flow
	}
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	if (!PS || PS->bAliveInRound || GetViewTarget() != EliminatedPawn)
	{
		return;   // only dead spectators currently viewing the eliminated pawn
	}

	TArray<APaintForgeCharacter*> Teammates;
	GatherLivingTeammatePawns(Teammates);
	if (Teammates.Num() > 0)
	{
		SpectateNearestTeammate();
	}
	else if (APawn* MyPawn = GetPawn())
	{
		SetViewTargetWithBlend(MyPawn, 0.25f);   // no teammates left: back to own body
	}
}

void APaintForgePlayerController::ServerSpectateNext_Implementation(bool bForward)
{
	const APaintForgeGameState* GS = GetPFGameState();
	const APaintForgePlayerState* PS = GetPlayerState<APaintForgePlayerState>();
	if (!GS || !PS || GS->Phase != EPFMatchPhase::Combat || PS->bAliveInRound)
	{
		return;   // dead-only
	}

	TArray<APaintForgeCharacter*> Teammates;
	GatherLivingTeammatePawns(Teammates);
	if (Teammates.Num() == 0)
	{
		return;
	}

	SpectateIndex = (SpectateIndex + (bForward ? 1 : -1) + Teammates.Num()) % Teammates.Num();
	SetViewTargetWithBlend(Teammates[SpectateIndex], 0.25f);
}

void APaintForgePlayerController::GatherLivingTeammatePawns(TArray<APaintForgeCharacter*>& OutPawns) const
{
	OutPawns.Reset();
	const APaintForgeGameState* GS = GetPFGameState();
	const APaintForgePlayerState* MyPS = GetPlayerState<APaintForgePlayerState>();
	if (!GS || !MyPS)
	{
		return;
	}
	// Sorted by roster index for a stable, deterministic cycle order.
	TArray<APaintForgePlayerState*> Sorted;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		APaintForgePlayerState* PS = Cast<APaintForgePlayerState>(PSBase);
		if (PS && PS != MyPS && PS->TeamId == MyPS->TeamId && PS->bAliveInRound)
		{
			Sorted.Add(PS);
		}
	}
	Sorted.Sort([](const APaintForgePlayerState& A, const APaintForgePlayerState& B)
	{
		return A.RosterIndex < B.RosterIndex;
	});
	for (APaintForgePlayerState* PS : Sorted)
	{
		if (APaintForgeCharacter* RosterPawn = Cast<APaintForgeCharacter>(PS->GetPawn()))
		{
			OutPawns.Add(RosterPawn);
		}
	}
}
