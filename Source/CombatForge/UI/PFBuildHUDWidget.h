// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/CombatForgeTypes.h"
#include "PFBuildHUDWidget.generated.h"

class ACombatForgeCharacter;
class ACombatForgePlayerState;
class UImage;
class UPFBuildComponent;
class UTextBlock;

/**
 * BuildPhase HUD (contract §3.6):
 *  - bottom-right budget readout "▦ N/30  ◆ N/6" (PlayerState OnFlagsChangedEvent)
 *  - equipped-piece name (UPFBuildComponent::OnEquippedToolChangedEvent)
 *  - phase countdown, polled from GameState::GetPhaseTimeRemaining per tick,
 *    flash at the 30 s / 10 s warnings (T2)
 *  - per-team ready counts "Ready 3/4 — 2/4" (0.5 s roster poll)
 *  - deny flash on OnPlaceDeniedEvent
 *  - center aim reticle (cross + dot) — combat HUD is swapped out in Build
 * Pawn wiring is pushed in by UPFRootHUDWidget (BindToPawn) on possession change.
 */
UCLASS()
class COMBATFORGE_API UPFBuildHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Rebind to a (possibly null) pawn; unbinds any previous pawn's components first. */
	void BindToPawn(ACombatForgeCharacter* NewPawn);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	void UnbindPawn();
	void BindLocalPlayerState();

	void HandleEquippedToolChanged(EPFBuildTool NewTool);
	void HandlePlaceDenied(EPFDenyReason Reason);
	void HandlePlayerStateFlagsChanged();

	void UpdateBudgetText();
	void UpdateReadyCounts();

	static const TCHAR* ToolDisplayName(EPFBuildTool Tool);
	static const TCHAR* DenyReasonText(EPFDenyReason Reason);

	UPROPERTY() TObjectPtr<UTextBlock> TimerText;
	/** Build-mode callout under the timer (Creative / Improvement / …). */
	UPROPERTY() TObjectPtr<UTextBlock> ModeBannerText;
	UPROPERTY() TObjectPtr<UTextBlock> ReadyText;
	UPROPERTY() TObjectPtr<UTextBlock> BudgetText;
	UPROPERTY() TObjectPtr<UTextBlock> EquippedText;
	UPROPERTY() TObjectPtr<UTextBlock> DenyText;
	UPROPERTY() TObjectPtr<UTextBlock> HintText;

	/** Placement aim reticle (combat crosshair is not on-screen during Build). */
	UPROPERTY() TObjectPtr<UImage> AimDot;
	UPROPERTY() TObjectPtr<UImage> AimLineH;
	UPROPERTY() TObjectPtr<UImage> AimLineV;

	TWeakObjectPtr<UPFBuildComponent> BoundBuild;
	TWeakObjectPtr<ACombatForgePlayerState> BoundPlayerState;

	float DenyFlashRemaining = 0.f;
	float WarnPulseRemaining = 0.f;
	float PrevPhaseRemaining = -1.f;
	float PollAccum = 0.f;

	static constexpr float PollInterval = 0.5f;
	static constexpr float DenyFlashDuration = 0.8f;
	static constexpr float WarnPulseDuration = 1.5f;
};
