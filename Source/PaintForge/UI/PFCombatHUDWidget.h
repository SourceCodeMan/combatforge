// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/PaintForgeTypes.h"
#include "PFCombatHUDWidget.generated.h"

class APaintForgeCharacter;
class APaintForgeGameState;
class UImage;
class UPFHealthComponent;
class UPFWeaponComponent;
class UProgressBar;
class UTextBlock;

/**
 * CombatPhase HUD (contract §3.6):
 *  - live-spread crosshair (polls UPFWeaponComponent::GetCurrentSpreadHalfAngleDeg
 *    per tick, projected through the live camera FOV); hidden in ADS -> 2 px dot
 *  - hopper "100/∞" + reload bar
 *  - round pips (first-to-4), round timer, alive counts per team
 *  - own HP as 3 paint dots
 *  - elim feed (last 4 lines), freeze/intermission/sudden-death banners
 */
UCLASS()
class PAINTFORGE_API UPFCombatHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Rebind to a (possibly null) pawn; unbinds any previous pawn's components first. */
	void BindToPawn(APaintForgeCharacter* NewPawn);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	void UnbindPawn();
	void TryBindGameState();

	// GameState delegate handlers.
	void HandleScoreChanged();
	void HandleAliveCountsChanged();
	void HandleElimFeedChanged();
	void HandleRoundStateChanged(EPFRoundState NewState);

	// Pawn component handlers.
	void HandleHopperChanged(int32 NewCount);
	void HandleReloadStateChanged(bool bNowReloading);
	void HandleHPChanged(uint8 NewHP);

	void UpdateCrosshair();
	void UpdateBanner(float InDeltaTime);

	UPROPERTY() TObjectPtr<UImage> CrossLineTop;
	UPROPERTY() TObjectPtr<UImage> CrossLineBottom;
	UPROPERTY() TObjectPtr<UImage> CrossLineLeft;
	UPROPERTY() TObjectPtr<UImage> CrossLineRight;
	UPROPERTY() TObjectPtr<UImage> CenterDot;

	UPROPERTY() TObjectPtr<UTextBlock> HopperText;
	UPROPERTY() TObjectPtr<UProgressBar> ReloadBar;
	UPROPERTY() TObjectPtr<UTextBlock> RoundTimerText;
	UPROPERTY() TObjectPtr<UTextBlock> RoundNumberText;
	UPROPERTY() TObjectPtr<UTextBlock> AliveTextA;
	UPROPERTY() TObjectPtr<UTextBlock> AliveTextB;
	UPROPERTY() TObjectPtr<UTextBlock> BannerText;
	UPROPERTY() TArray<TObjectPtr<UImage>> PipsA;
	UPROPERTY() TArray<TObjectPtr<UImage>> PipsB;
	UPROPERTY() TArray<TObjectPtr<UImage>> HPDots;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> FeedLines;

	TWeakObjectPtr<APaintForgeCharacter> BoundPawn;
	TWeakObjectPtr<UPFWeaponComponent> BoundWeapon;
	TWeakObjectPtr<UPFHealthComponent> BoundHealth;
	TWeakObjectPtr<APaintForgeGameState> BoundGameState;

	bool bReloading = false;
	float ReloadElapsed = 0.f;
	/** Keeps the "GO!" banner up briefly after Live starts. */
	float BannerHoldRemaining = 0.f;
	EPFRoundState LastRoundState = EPFRoundState::None;

	static constexpr int32 MaxPips = 4;
	static constexpr int32 MaxHP = 3;
	static constexpr int32 FeedLineCount = 4;
	static constexpr float CrosshairBaseGapPx = 6.f;
	static constexpr float CrossLineLengthPx = 12.f;
	static constexpr float CrossLineThicknessPx = 2.f;
};
