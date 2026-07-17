// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/CombatForgeTypes.h"
#include "PFRootHUDWidget.generated.h"

class ACombatForgeCharacter;
class ACombatForgeGameState;
class ACombatForgePlayerController;
class UPFBuildComponent;
class UPFBuildHUDWidget;
class UPFBuildWheelWidget;
class UPFCombatFeedbackWidget;
class UPFCombatHUDWidget;
class UPFLobbyWidget;
class UPFOptionsWidget;
class UPFResultsWidget;
class UPFScoreboardWidget;
class UPFVoteWidget;
class UWidgetSwitcher;

/**
 * Viewport root (contract §3.6): created exactly once by the PC in BeginPlayingState (local
 * controllers only). Owns a UWidgetSwitcher of phase panels — child index == (int32)EPFMatchPhase
 * (Lobby..Results = 0..4) — plus the persistent overlays (combat feedback, build wheel, hold-Tab
 * scoreboard, options menu). Panels switch on GameState OnPhaseChangedEvent.
 *
 * CROSS-PACKAGE WIRING DUTY (binding, §3.6):
 *  - BuildComponent->OnBuildWheelRequestedEvent -> Wheel->Open() / Wheel->CloseAndCommit() (Q tap)
 *  - Wheel->OnToolSelectedEvent -> BuildComponent->EquipTool()
 *  - Wheel->OnWheelClosedEvent -> BuildComponent->NotifyBuildWheelClosed()
 *  - re-wired on pawn change via the possession delegate — child widgets never cache pawn
 *    pointers across possession (the pawn is pushed down through BindToPawn)
 *  - scoreboard visibility from ACombatForgePlayerController::IsScoreboardHeld() /
 *    OnScoreboardHeldChanged (pkg-core's contract-gap seam)
 *  - Escape / lobby OPTIONS -> ToggleOptions()
 */
UCLASS()
class COMBATFORGE_API UPFRootHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Open / close / toggle the full-screen options overlay (video, audio, controls). */
	void OpenOptions();
	void CloseOptions();
	void ToggleOptions();
	bool IsOptionsOpen() const;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Possession delegate handler (APlayerController::OnPossessedPawnChanged is dynamic). */
	UFUNCTION() void HandlePossessedPawnChanged(APawn* OldPawn, APawn* NewPawn);

private:
	void BuildTree();
	void TryBindGameState();
	void BindOwningPC();
	void UnbindOwningPC();

	void HandlePhaseChanged(EPFMatchPhase NewPhase);
	void HandleScoreboardHeldChanged(bool bHeld);
	void HandleBuildWheelRequested(bool bOpen);
	void HandleWheelToolSelected(EPFBuildTool Tool);
	void HandleWheelClosed(bool bUnused);

	/** Unwires the previous pawn's components and pushes the new pawn into the child widgets. */
	void WirePawn(ACombatForgeCharacter* NewPawn);
	void UpdateScoreboardVisibility();

	ACombatForgePlayerController* GetPFPlayerController() const;

	// Phase panels + persistent overlays (contract §3.6) — all created in RebuildWidget.
	UPROPERTY() TObjectPtr<UWidgetSwitcher> PhaseSwitcher;
	UPROPERTY() TObjectPtr<UPFLobbyWidget> LobbyPanel;
	UPROPERTY() TObjectPtr<UPFBuildHUDWidget> BuildPanel;
	UPROPERTY() TObjectPtr<UPFCombatHUDWidget> CombatPanel;
	UPROPERTY() TObjectPtr<UPFVoteWidget> VotePanel;
	UPROPERTY() TObjectPtr<UPFResultsWidget> ResultsPanel;
	UPROPERTY() TObjectPtr<UPFBuildWheelWidget> WheelWidget;
	UPROPERTY() TObjectPtr<UPFCombatFeedbackWidget> FeedbackWidget;
	UPROPERTY() TObjectPtr<UPFScoreboardWidget> ScoreboardWidget;
	UPROPERTY() TObjectPtr<UPFOptionsWidget> OptionsWidget;

	TWeakObjectPtr<ACombatForgeGameState> BoundGameState;
	TWeakObjectPtr<UPFBuildComponent> BoundBuild;
	TWeakObjectPtr<ACombatForgeCharacter> WiredPawn;
	bool bPCBound = false;
};
