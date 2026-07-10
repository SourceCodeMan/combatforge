// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Core/PaintForgeTypes.h"
#include "PaintForgePlayerController.generated.h"

class APaintForgeCharacter;
class APaintForgeGameState;
class APaintForgePlayerState;
class UPFInputConfig;
class UPFRootHUDWidget;

/**
 * Player controller (contract §3.2): builds the native input objects (UPFInputConfig), applies
 * mapping contexts + input modes per phase (§4.5 table), creates the root HUD on local
 * controllers, carries the ready/vote/host/guid RPCs, and runs the death-cam → teammate
 * spectate flow (T5).
 */
UCLASS()
class PAINTFORGE_API APaintForgePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// ---- RPCs (client → server, all Reliable) ----
	UFUNCTION(Server, Reliable) void ServerSetReady(bool bNewReady);
	UFUNCTION(Server, Reliable) void ServerSubmitVote(EPFThumbVote Thumb,
	                                const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds);
	UFUNCTION(Server, Reliable) void ServerSetPlayerGuidHash(const FString& GuidHash); // once, on join
	UFUNCTION(Server, Reliable) void ServerHostForceStart();      // ignored if not host
	UFUNCTION(Server, Reliable) void ServerHostCycleTeam(APaintForgePlayerState* Target);
	UFUNCTION(Server, Reliable) void ServerHostReturnToLobby();
	UFUNCTION(Server, Reliable) void ServerSpectateNext(bool bForward); // dead-only; server retargets ViewTarget

	// ---- Cross-package accessors ----
	UPFInputConfig* GetInputConfig() const;   // never null after SetupInputComponent

	// ---- pkg-core intra API (GameMode → PC, server side) ----
	void StartDeathCamera();                  // T5: 0.5 s locked death cam, then nearest-teammate spectate
	void ApplyServerMoveLock(bool bLocked);   // server-copy move-input freeze (Freeze/Intermission/Vote/Results)
	void SetEliminatedMoveLock(bool bLocked); // server: victim move freeze for the rest of the round
	                                          // (mirrored to the owning client via ClientSetEliminatedMoveLock)
	void RetargetSpectatorFrom(APawn* EliminatedPawn); // server: spectated pawn was eliminated → move on

	// CONTRACT-GAP: no §3 API exists for the PC to drive scoreboard visibility inside
	// UPFRootHUDWidget; exposing the held state here is the smallest seam (pkg-ui may poll it).
	bool IsScoreboardHeld() const { return bScoreboardHeld; }
	TMulticastDelegate<void(bool /*bHeld*/)> OnScoreboardHeldChanged;

protected:
	// ---- Engine overrides ----
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;
	virtual void BeginPlayingState() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- (intra) phase reaction ----
	void TryBindGameState();
	void HandlePhaseChanged(EPFMatchPhase NewPhase);
	void HandleRoundStateChanged(EPFRoundState NewState);
	void ApplyInputForPhase();                // IMCs + input mode + local move ignore per §4.5
	void ApplyLocalMoveLock(bool bLocked);    // phase-driven lock; combined with the elim lock
	void RefreshMoveLock();                   // re-applies phase lock OR elim lock idempotently

	// Owning-client mirror of the elimination move freeze (move-input ignore does not replicate).
	UFUNCTION(Client, Reliable) void ClientSetEliminatedMoveLock(bool bLocked);
	void CreateHUDIfNeeded();
	void TrySendGuidHash();

	// ---- (intra) input handlers (PC-level actions) ----
	void OnReadyToggle();
	void OnHostStartPressed();
	void OnScoreboardStarted();
	void OnScoreboardCompleted();
	void OnMenuBack();
	void OnFireWhileDead();                   // dead-only spectate cycle: Fire = next (T5)
	void OnADSWhileDead();                    // ADS = previous

	// ---- (intra) spectate (server side) ----
	void SpectateNearestTeammate();
	void GatherLivingTeammatePawns(TArray<APaintForgeCharacter*>& OutPawns) const;
	bool IsHostController() const;
	APaintForgeGameState* GetPFGameState() const;

private:
	UPROPERTY() TObjectPtr<UPFInputConfig>    InputConfig;   // GC root for all input objects (02 R1)
	UPROPERTY() TObjectPtr<UPFRootHUDWidget>  RootHUD;

	FTimerHandle InputRetryHandle;      // Enhanced Input subsystem may not exist at first call (02 R1)
	FTimerHandle GameStateRetryHandle;
	FTimerHandle DeathCamHandle;

	bool bGameStateBound = false;
	bool bGuidHashSent = false;
	bool bScoreboardHeld = false;
	bool bPhaseMoveLock = false;        // §4.5 phase/round-state move freeze
	bool bEliminatedMoveLock = false;   // dead-for-the-round move freeze (each side keeps its copy)
	int32 SpectateIndex = 0;            // server-side cycle cursor into living-teammate list
};
