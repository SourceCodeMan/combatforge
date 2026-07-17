// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/CombatForgeTypes.h"
#include "PFCombatHUDWidget.generated.h"

class ACombatForgeCharacter;
class ACombatForgeGameState;
class UImage;
class UPFHealthComponent;
class UPFWeaponComponent;
class UProgressBar;
class USizeBox;
class UTextBlock;

/**
 * CombatPhase HUD (contract §3.6):
 *  - live-spread crosshair (polls UPFWeaponComponent::GetCurrentSpreadHalfAngleDeg
 *    per tick, projected through the live camera FOV); hidden in ADS -> 2 px dot
 *  - hopper "100/∞" + reload bar
 *  - round pips (first-to-4; first-to-3 at <=2v2 per T15), round timer, alive counts per team
 *  - own HP as 3 paint dots
 *  - elim feed (last 4 lines), freeze/intermission/sudden-death banners
 *  - "YOU'RE OUT" full-screen when eliminated + respawn countdown (or out-for-round)
 */
UCLASS()
class COMBATFORGE_API UPFCombatHUDWidget : public UUserWidget
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
	void TryBindGameState();

	// GameState delegate handlers.
	void HandleScoreChanged();
	void HandleAliveCountsChanged();
	void HandleElimFeedChanged();
	void HandleRoundStateChanged(EPFRoundState NewState);

	// Pawn component handlers.
	void HandleHopperChanged(int32 NewCount);
	void HandleReloadStateChanged(bool bNowReloading);
	void HandleHitsChanged(uint8 HeadHits, uint8 ChestHits, uint8 LimbHits, uint8 TotalHits);
	void HandleFireModeChanged(EPFFireMode NewMode);
	void HandleGrenadeCountChanged(uint8 Frag, uint8 Smoke);
	/** Polls bound pawn bCarryingBomb (replicated) — show/hide BOMB · G. */
	void UpdateBombCarryIndicator();

	void UpdateCrosshair();
	void UpdateBanner(float InDeltaTime);
	/** CTF carrier / Dom-HP on-point strip (reads local PlayerState each tick). */
	void UpdateObjectiveStatus();
	void UpdateDominationHUD();   // A/B/C ownership chips + the capture-progress bar
	void EnsureZoneCache();       // find/cache the three control-point actors (clients get them via replication)
	/** Full-screen "YOU'RE OUT" + respawn timer / out-for-round (local eliminated player only). */
	void UpdateOutOverlay();

	/** Collapses the center-nearest pips so only the effective wins-to-take show (T15). */
	void UpdatePipVisibility(int32 EffectiveCount);

	UPROPERTY() TObjectPtr<UImage> CrossLineTop;
	UPROPERTY() TObjectPtr<UImage> CrossLineBottom;
	UPROPERTY() TObjectPtr<UImage> CrossLineLeft;
	UPROPERTY() TObjectPtr<UImage> CrossLineRight;
	UPROPERTY() TObjectPtr<UImage> CenterDot;

	UPROPERTY() TObjectPtr<UTextBlock> HopperText;
	UPROPERTY() TObjectPtr<UProgressBar> ReloadBar;
	UPROPERTY() TObjectPtr<UTextBlock> FireModeText;
	UPROPERTY() TObjectPtr<UTextBlock> GrenadeText;
	/** Visible only while the local pawn holds a mid-field bomb charge (G to plant). */
	UPROPERTY() TObjectPtr<UTextBlock> BombCarryText;
	UPROPERTY() TObjectPtr<UTextBlock> RoundTimerText;
	UPROPERTY() TObjectPtr<UTextBlock> RoundNumberText;
	UPROPERTY() TObjectPtr<UTextBlock> AliveTextA;
	UPROPERTY() TObjectPtr<UTextBlock> AliveTextB;
	UPROPERTY() TObjectPtr<UTextBlock> BannerText;
	/** Objective callout under the top score strip (carrier / on-point). Hidden when idle. */
	UPROPERTY() TObjectPtr<UTextBlock> ObjectiveStatusText;
	// Domination: A/B/C zone chips under the score strip (letter = owner color, "|··" segment bar while
	// capturing) + the CoD-style linear capture meter above the weapon HUD when the local player is in a zone.
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ZoneChips;
	UPROPERTY() TObjectPtr<class UHorizontalBox> ZoneChipsRow;
	UPROPERTY() TObjectPtr<UProgressBar> CaptureBar;
	UPROPERTY() TObjectPtr<UTextBlock> CaptureBarLabel;
	TArray<TWeakObjectPtr<class APFControlPointActor>> ZoneCache;

	/** Full-screen elim overlay (dim + YOU'RE OUT + countdown). */
	UPROPERTY() TObjectPtr<UImage> OutDim;
	UPROPERTY() TObjectPtr<UTextBlock> OutTitleText;
	UPROPERTY() TObjectPtr<UTextBlock> OutSubtitleText;
	UPROPERTY() TObjectPtr<UTextBlock> OutClassText;     // "CLASS n — <weapon> | scroll to change" on the countdown

	UPROPERTY() TArray<TObjectPtr<UImage>> PipsA;
	UPROPERTY() TArray<TObjectPtr<UImage>> PipsB;
	UPROPERTY() TArray<TObjectPtr<USizeBox>> PipSizersA;
	UPROPERTY() TArray<TObjectPtr<USizeBox>> PipSizersB;
	UPROPERTY() TArray<TObjectPtr<UImage>> HPDots;
	UPROPERTY() TObjectPtr<UTextBlock> RegionHitsText;   // "H 1/3 · C 2/5 · L 0/8" under the pip row
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> FeedLines;

	TWeakObjectPtr<ACombatForgeCharacter> BoundPawn;
	TWeakObjectPtr<UPFWeaponComponent> BoundWeapon;
	TWeakObjectPtr<UPFHealthComponent> BoundHealth;
	TWeakObjectPtr<ACombatForgeGameState> BoundGameState;

	bool bReloading = false;
	float ReloadElapsed = 0.f;
	/** Keeps the "GO!" banner up briefly after Live starts. */
	float BannerHoldRemaining = 0.f;
	EPFRoundState LastRoundState = EPFRoundState::None;

	/** Pips are built at the first-to-4 max; small formats (T15) collapse the extras. */
	static constexpr int32 MaxPips = 4;
	/** <=2v2 (<=4 players) plays first-to-3 (T15) — GameMode config isn't replicated, so infer. */
	static constexpr int32 SmallFormatPips = 3;
	static constexpr int32 SmallFormatMaxPlayers = 4;
	// Locational hit model: 10 total-hit pips (out at 10 anywhere; 3 head / 5 chest / 8 limbs first).
	static constexpr int32 MaxHitPips = 10;
	static constexpr int32 FeedLineCount = 4;
	static constexpr float CrosshairBaseGapPx = 6.f;
	static constexpr float CrossLineLengthPx = 12.f;
	static constexpr float CrossLineThicknessPx = 2.f;
};
