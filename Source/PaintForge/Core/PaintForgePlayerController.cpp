// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PaintForgePlayerController.h"

#include "PaintForge.h"
#include "Core/PaintForgeGameInstance.h"
#include "Core/PaintForgeGameMode.h"
#include "Core/PaintForgeGameState.h"
#include "Core/PaintForgePlayerState.h"
#include "Core/PFClientLogShip.h"
#include "Player/PaintForgeCharacter.h"
#include "Input/PFInputConfig.h"
#include "UI/PFRootHUDWidget.h"
#include "UI/PFLoadingMenuWidget.h"
#include "Combat/PFCombatAudio.h"

#include "Blueprint/UserWidget.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/InputSettings.h"
#include "HAL/FileManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

namespace
{
	constexpr float InputRetryInterval = 0.25f;   // 02 R1: subsystem may not exist at first call
	constexpr float DeathCamDuration   = 0.5f;    // T5: locked death cam before teammate spectate
	constexpr float ClientLogShipInterval = 1.0f; // flush staged client logs to host each second
	constexpr int32 ClientLogChunkMaxChars = 1800; // stay under reliable RPC comfort size
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void APaintForgePlayerController::SetupInputComponent()
{
	// Packaged / -game launches sometimes create a plain UInputComponent despite the plugin
	// being enabled. Force Enhanced Input before Super so Character bindings can Cast<> it.
	if (InputComponent && !InputComponent->IsA<UEnhancedInputComponent>())
	{
		InputComponent->DestroyComponent();
		InputComponent = nullptr;
	}
	if (InputComponent == nullptr)
	{
		InputComponent = NewObject<UEnhancedInputComponent>(this, TEXT("PC_EnhancedInputComponent"));
		InputComponent->RegisterComponent();
	}

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
		UE_LOG(PaintForgeLog, Error, TEXT("PC: EnhancedInputComponent missing - PC-level actions unbound (InputComponent=%s)"),
			InputComponent ? *InputComponent->GetClass()->GetName() : TEXT("null"));
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
		// Boot menu first (opaque) so the player never stares at a half-loaded pen + HUD.
		CreateLoadingMenuIfNeeded();
		CreateHUDIfNeeded();
		// While the loading menu is up it owns input mode; otherwise apply lobby/combat IMCs.
		if (LoadingMenu == nullptr || LoadingMenu->IsFinished())
		{
			ApplyInputForPhase();
		}
		TrySendGuidHash();
		StartClientLogShip();   // remote clients only — tee GLog → host Saved/ClientLogs
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
	// Best-effort flush before the connection dies (won't run on hard crash — periodic ship covers that).
	FlushClientLogShip();
	StopClientLogShip();

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
		World->GetTimerManager().ClearTimer(ClientLogShipTimer);
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
	// Seed LastSeenRoundState BEFORE HandleRoundStateChanged so join-into-Live does not
	// fire a false breakout horn (pre-playtest finding #4).
	LastSeenRoundState = GS->RoundState;
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

	// Breakout horn only on a real Freeze → Live transition (T6), never on join catch-up
	// or Live re-broadcasts. Local pawn only.
	const bool bBreakout = (NewState == EPFRoundState::Live
		&& LastSeenRoundState == EPFRoundState::Freeze);
	LastSeenRoundState = NewState;
	if (bBreakout)
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
	// Boot menu owns input until dismissed — don't let phase IMCs fight the UI-only mode.
	if (LoadingMenu && !LoadingMenu->IsFinished())
	{
		return;
	}

	// Options overlay owns GameAndUI + cursor while open.
	if (RootHUD && RootHUD->IsOptionsOpen())
	{
		return;
	}

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
		// Always show a cursor in lobby so match-setup UI is clickable without Tab.
		// (Tab still toggles the full scoreboard overlay.)
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

	// Input mode + cursor (§4.5). Lobby always GameAndUI so first-time hosts can click
	// MODE/TYPE/FORMAT without knowing Tab; combat stays GameOnly for look capture.
	if (bUIOnly)
	{
		SetInputMode(FInputModeUIOnly());
		SetShowMouseCursor(true);
	}
	else if (GS->Phase == EPFMatchPhase::Lobby)
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(Mode);
		SetShowMouseCursor(true);
	}
	else
	{
		FInputModeGameOnly Mode;
		Mode.SetConsumeCaptureMouseDown(true);
		SetInputMode(Mode);
		SetShowMouseCursor(false);
	}

	// Standalone -game launches from PowerShell often leave keyboard/mouse focus on the console
	// window — without this the game looks "frozen" (no look, no keys).
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport(EFocusCause::SetDirectly);
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

void APaintForgePlayerController::CreateLoadingMenuIfNeeded()
{
	if (LoadingMenu || !IsLocalController())
	{
		return;
	}
	LoadingMenu = CreateWidget<UPFLoadingMenuWidget>(this, UPFLoadingMenuWidget::StaticClass());
	if (LoadingMenu)
	{
		// Z-order above RootHUD so the opaque menu fully covers the live world + lobby UI.
		LoadingMenu->AddToViewport(100);
		UE_LOG(PaintForgeLog, Log, TEXT("PC: loading menu shown (shader/asset warmup)"));
	}
	else
	{
		UE_LOG(PaintForgeLog, Error, TEXT("PC: failed to create UPFLoadingMenuWidget"));
	}
}

void APaintForgePlayerController::NotifyLoadingMenuFinished()
{
	ApplyInputForPhase();
}

void APaintForgePlayerController::NotifyOptionsMenuClosed()
{
	if (!IsLocalController())
	{
		return;
	}
	ApplyInputForPhase();
}

void APaintForgePlayerController::ToggleOptionsMenu()
{
	if (!IsLocalController())
	{
		return;
	}
	// Don't cover the boot loading menu — finish warmup first.
	if (LoadingMenu && !LoadingMenu->IsFinished())
	{
		return;
	}
	CreateHUDIfNeeded();
	if (RootHUD)
	{
		RootHUD->ToggleOptions();
	}
}

bool APaintForgePlayerController::IsOptionsMenuOpen() const
{
	return RootHUD && RootHUD->IsOptionsOpen();
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
		// Under the loading menu (Z=100) until Enter Lobby is pressed.
		RootHUD->AddToViewport(0);
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
	// Escape toggles the options / pause settings overlay (video, audio, controls).
	ToggleOptionsMenu();
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

void APaintForgePlayerController::ServerHostSetFillWithBots_Implementation(bool bFill)
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostSetFillWithBots(bFill);
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

void APaintForgePlayerController::ServerHostSetCommunityMap_Implementation(const FString& FileName,
	const FString& Label)
{
	if (!IsHostController())
	{
		return;
	}
	if (APaintForgeGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<APaintForgeGameMode>() : nullptr)
	{
		GM->HostSetCommunityMap(FileName, Label);
	}
}

// ---------------------------------------------------------------------------
// Client → host log ship (LAN crash triage)
// ---------------------------------------------------------------------------

void APaintForgePlayerController::StartClientLogShip()
{
	// Only pure remote clients — listen-host already has its own Saved/Logs on disk.
	if (!IsLocalController() || GetNetMode() != NM_Client)
	{
		return;
	}
	if (ClientLogCapture.IsValid())
	{
		return;
	}
	ClientLogCapture = MakeUnique<FPFClientLogCapture>();
	if (GLog)
	{
		GLog->AddOutputDevice(ClientLogCapture.Get());
	}
	GetWorldTimerManager().SetTimer(ClientLogShipTimer, this,
		&APaintForgePlayerController::TickClientLogShip, ClientLogShipInterval, /*bLoop=*/true);
	UE_LOG(PaintForgeLog, Log, TEXT("ClientLogShip: started (shipping to host every %.1fs)"),
		ClientLogShipInterval);
	// Immediate banner so the host file is non-empty even if the client dies early.
	ServerShipClientLog(FString::Printf(
		TEXT("=== ClientLogShip start machine=%s player=%s ===\n"),
		FPlatformProcess::ComputerName(),
		GetPlayerState<APlayerState>() ? *GetPlayerState<APlayerState>()->GetPlayerName() : TEXT("?")));
}

void APaintForgePlayerController::StopClientLogShip()
{
	if (ClientLogCapture.IsValid() && GLog)
	{
		GLog->RemoveOutputDevice(ClientLogCapture.Get());
	}
	ClientLogCapture.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ClientLogShipTimer);
	}
}

void APaintForgePlayerController::TickClientLogShip()
{
	FlushClientLogShip();
}

void APaintForgePlayerController::FlushClientLogShip()
{
	if (!ClientLogCapture.IsValid() || !IsLocalController() || GetNetMode() != NM_Client)
	{
		return;
	}
	// Drain in multiple chunks if backlog is large (e.g. after a hitch).
	for (int32 i = 0; i < 8; ++i)
	{
		const FString Chunk = ClientLogCapture->TakeChunk(ClientLogChunkMaxChars);
		if (Chunk.IsEmpty())
		{
			break;
		}
		ServerShipClientLog(Chunk);
	}
}

bool APaintForgePlayerController::ServerShipClientLog_Validate(const FString& Chunk)
{
	// Reject absurd payloads (DoS / bad client). Normal chunks are ~1–2 KB.
	return Chunk.Len() <= 4000;
}

void APaintForgePlayerController::ServerShipClientLog_Implementation(const FString& Chunk)
{
	if (!HasAuthority() || Chunk.IsEmpty())
	{
		return;
	}
	if (ServerClientLogPath.IsEmpty())
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("ClientLogs");
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		FString Name = TEXT("client");
		if (const APlayerState* PS = PlayerState)
		{
			Name = PS->GetPlayerName();
		}
		// Filesystem-safe name.
		Name = Name.Replace(TEXT(" "), TEXT("_"));
		Name = Name.Replace(TEXT(":"), TEXT("-"));
		Name = Name.Replace(TEXT("/"), TEXT("-"));
		Name = Name.Replace(TEXT("\\"), TEXT("-"));
		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		ServerClientLogPath = Dir / FString::Printf(TEXT("%s_%s.log"), *Name, *Stamp);
		const FString Header = FString::Printf(
			TEXT("=== Host received client log for %s @ %s ===\n"),
			*Name, *FDateTime::Now().ToIso8601());
		FFileHelper::SaveStringToFile(Header, *ServerClientLogPath);
		UE_LOG(PaintForgeLog, Log, TEXT("ClientLogShip: writing remote log -> %s"), *ServerClientLogPath);
	}
	FFileHelper::SaveStringToFile(Chunk, *ServerClientLogPath,
		FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
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

void APaintForgePlayerController::PFForceStart()
{
	// Host convenience / smoke: same as Enter force-start in the lobby.
	ServerHostForceStart();
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
