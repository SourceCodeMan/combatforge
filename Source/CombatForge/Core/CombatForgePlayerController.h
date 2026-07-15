// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Core/CombatForgeTypes.h"
#include "Core/PFClientLogShip.h"
#include "CombatForgePlayerController.generated.h"

class ACombatForgeCharacter;
class ACombatForgeGameState;
class ACombatForgePlayerState;
class UPFInputConfig;
class UPFRootHUDWidget;
class UPFLoadingMenuWidget;

/**
 * Player controller (contract §3.2): builds the native input objects (UPFInputConfig), applies
 * mapping contexts + input modes per phase (§4.5 table), creates the root HUD on local
 * controllers, carries the ready/vote/host/guid RPCs, and runs the death-cam → teammate
 * spectate flow (T5).
 */
UCLASS()
class COMBATFORGE_API ACombatForgePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// ---- RPCs (client → server, all Reliable) ----
	UFUNCTION(Server, Reliable) void ServerSetReady(bool bNewReady);
	UFUNCTION(Server, Reliable) void ServerSubmitVote(EPFThumbVote Thumb,
	                                const TArray<uint8>& LikedIds, const TArray<uint8>& DislikedIds);
	UFUNCTION(Server, Reliable) void ServerSetPlayerGuidHash(const FString& GuidHash); // once, on join
	UFUNCTION(Server, Reliable) void ServerHostForceStart();      // ignored if not host
	UFUNCTION(Server, Reliable) void ServerHostCycleTeam(ACombatForgePlayerState* Target);
	UFUNCTION(Server, Reliable) void ServerHostReturnToLobby();
	/** Mid-match quit-to-menu (host only). Clients should disconnect instead. */
	UFUNCTION(Server, Reliable) void ServerHostForceReturnToLobby();
	UFUNCTION(Server, Reliable) void ServerHostSetFormat(uint8 TeamSize);   // Lobby only: 4v4 / 6v6
	UFUNCTION(Server, Reliable) void ServerHostSetFillWithBots(bool bFill); // Lobby only: fill roster with bots
	UFUNCTION(Server, Reliable) void ServerHostSetBuildMode(uint8 Mode);    // Lobby only: 0=Creative 1=Improvement 2=PlayOnly
	UFUNCTION(Server, Reliable) void ServerHostSetMatchType(uint8 Type);    // Lobby only: 0=Elim 1=FFA 2=Skirmish 3=CTF 4=Dom 5=Hardpoint
	/** Lobby only: host community map pick (filename under Saved/Arenas/, empty = auto). */
	UFUNCTION(Server, Reliable) void ServerHostSetCommunityMap(const FString& FileName, const FString& Label);
	/** Remote client → host: append a log chunk under Saved/ClientLogs/ (LAN crash triage). */
	UFUNCTION(Server, Reliable, WithValidation) void ServerShipClientLog(const FString& Chunk);

	// Console convenience for the host to configure the match in the lobby.
	UFUNCTION(Exec) void PFFormat(int32 TeamSize);   // "PFFormat 6"
	UFUNCTION(Exec) void PFMode(int32 Mode);         // "PFMode 2" (Play-only)
	UFUNCTION(Exec) void PFType(int32 Type);         // "PFType 0" (Elimination)
	UFUNCTION(Exec) void PFForceStart();             // host: force lobby → match (smoke / playtest)
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

	/** Boot loading menu dismissed — re-apply phase IMCs / lobby GameAndUI. */
	void NotifyLoadingMenuFinished();

	/** Options overlay closed (Back / Esc) — restore phase input mode + cursor. */
	void NotifyOptionsMenuClosed();

	/** Open / close / toggle video-audio-controls options (local only). */
	void ToggleOptionsMenu();
	bool IsOptionsMenuOpen() const;

	/** Quit entire process (main menu). */
	void QuitToDesktop();
	/**
	 * Leave the match: host force-returns everyone to Lobby; remote clients disconnect.
	 * Safe from options / lobby.
	 */
	void QuitToMenu();

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
	/** Full-screen boot menu + shader warmup (covers live world until Enter). */
	void CreateLoadingMenuIfNeeded();
	void TrySendGuidHash();

	// ---- (intra) client → host log ship (LAN crash triage) ----
	void StartClientLogShip();
	void StopClientLogShip();
	void TickClientLogShip();
	void FlushClientLogShip();   // send remaining buffer (EndPlay / periodic)

	// ---- (intra) input handlers (PC-level actions) ----
	void OnReadyToggle();
	void OnHostStartPressed();
	void OnScoreboardStarted();
	void OnScoreboardCompleted();
	void OnMenuBack();
	void OnFireWhileDead();                   // dead-only spectate cycle: Fire = next (T5)
	void OnADSWhileDead();                    // ADS = previous
	void OnCycleClassWhileDead(const struct FInputActionValue& Value); // wheel on the respawn countdown = switch class

	// ---- (intra) spectate (server side) ----
	void SpectateNearestTeammate();
	void GatherLivingTeammatePawns(TArray<ACombatForgeCharacter*>& OutPawns) const;
	bool IsHostController() const;
	ACombatForgeGameState* GetPFGameState() const;

private:
	UPROPERTY() TObjectPtr<UPFInputConfig>    InputConfig;   // GC root for all input objects (02 R1)
	UPROPERTY() TObjectPtr<UPFRootHUDWidget>  RootHUD;
	UPROPERTY() TObjectPtr<UPFLoadingMenuWidget> LoadingMenu;

	FTimerHandle InputRetryHandle;      // Enhanced Input subsystem may not exist at first call (02 R1)
	FTimerHandle GameStateRetryHandle;
	FTimerHandle DeathCamHandle;
	FTimerHandle ClientLogShipTimer;

	/** Remote-client GLog tee; only live when NM_Client + local. */
	TUniquePtr<FPFClientLogCapture> ClientLogCapture;
	/** Server: append path for this connection's shipped client log. */
	FString ServerClientLogPath;

	bool bGameStateBound = false;
	bool bGuidHashSent = false;
	bool bScoreboardHeld = false;
	bool bPhaseMoveLock = false;        // §4.5 phase/round-state move freeze
	bool bEliminatedMoveLock = false;   // dead-for-the-round move freeze (each side keeps its copy)
	/** Last RoundState seen by this local PC — breakout only fires Freeze→Live, not join catch-up. */
	EPFRoundState LastSeenRoundState = EPFRoundState::None;
	int32 SpectateIndex = 0;            // server-side cycle cursor into living-teammate list
};
