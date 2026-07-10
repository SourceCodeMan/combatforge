// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/PaintForgeTypes.h"
#include "PFRootHUDWidget.generated.h"

class APaintForgeCharacter;
class APaintForgeGameState;
class APaintForgePlayerController;
class UPFBuildComponent;
class UPFBuildHUDWidget;
class UPFBuildWheelWidget;
class UPFCombatFeedbackWidget;
class UPFCombatHUDWidget;
class UPFLobbyWidget;
class UPFResultsWidget;
class UPFScoreboardWidget;
class UPFVoteWidget;
class UWidgetSwitcher;

/**
 * Viewport root (contract §3.6): created exactly once by the PC in BeginPlayingState (local
 * controllers only). Owns a UWidgetSwitcher of phase panels — child index == (int32)EPFMatchPhase
 * (Lobby..Results = 0..4) — plus the persistent overlays (combat feedback, build wheel, hold-Tab
 * scoreboard). Panels switch on GameState OnPhaseChangedEvent.
 *
 * CROSS-PACKAGE WIRING DUTY (binding, §3.6):
 *  - BuildComponent->OnBuildWheelRequestedEvent -> Wheel->Open() / Wheel->CloseAndCommit()
 *  - Wheel->OnToolSelectedEvent -> BuildComponent->EquipTool()
 *  - re-wired on pawn change via the possession delegate — child widgets never cache pawn
 *    pointers across possession (the pawn is pushed down through BindToPawn)
 *  - scoreboard visibility from APaintForgePlayerController::IsScoreboardHeld() /
 *    OnScoreboardHeldChanged (pkg-core's contract-gap seam)
 */
UCLASS()
class PAINTFORGE_API UPFRootHUDWidget : public UUserWidget
{
	GENERATED_BODY()

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

	/** Unwires the previous pawn's components and pushes the new pawn into the child widgets. */
	void WirePawn(APaintForgeCharacter* NewPawn);
	void UpdateScoreboardVisibility();

	APaintForgePlayerController* GetPFPlayerController() const;

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

	TWeakObjectPtr<APaintForgeGameState> BoundGameState;
	TWeakObjectPtr<UPFBuildComponent> BoundBuild;
	TWeakObjectPtr<APaintForgeCharacter> WiredPawn;
	bool bPCBound = false;
};
