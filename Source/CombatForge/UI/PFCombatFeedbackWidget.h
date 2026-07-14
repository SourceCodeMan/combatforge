// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/CombatForgeTypes.h"
#include "PFCombatFeedbackWidget.generated.h"

class ACombatForgeCharacter;
class ACombatForgeGameState;
class UCanvasPanel;
class UImage;
class UPFHealthComponent;
class UPFWeaponComponent;
class UTextBlock;

/**
 * Owning-client combat feedback (contract §3.6, 04 §4):
 *  - Weapon->OnHitConfirmedEvent -> hitmarker + PlayHitmarker()/PlayElim()
 *  - Health->OnLocalPaintHitTakenEvent -> damage arc, mask splats, camera punch, flash
 *  - "SPLATTED [name]" center text on own elim confirm
 * Pawn wiring is pushed in by UPFRootHUDWidget (BindToPawn) on possession change.
 */
UCLASS()
class COMBATFORGE_API UPFCombatFeedbackWidget : public UUserWidget
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

	void HandleHitConfirmed(uint32 ShotIndex, bool bElimHit);
	void HandleHitTaken(FVector ShooterLoc, uint8 ShooterTeam, uint8 NewHP);
	void HandleRoundStateChanged(EPFRoundState NewState);
	void HandleElimFeedChanged();

	void WipeMaskSplats();
	FString LookUpRecentVictimName() const;
	/** Yaw of the shooter relative to the local camera: 0 = ahead, clockwise positive. */
	float ComputeRelativeYawDeg(const FVector& ShooterLoc) const;

	// ---- Hitmarker ----
	UPROPERTY() TObjectPtr<UCanvasPanel> HitmarkerPanel;
	UPROPERTY() TArray<TObjectPtr<UImage>> HitmarkerTicks;
	float HitmarkerRemaining = 0.f;
	bool bHitmarkerElim = false;

	// ---- Damage direction arcs (pooled) ----
	UPROPERTY() TArray<TObjectPtr<UImage>> ArcImages;
	TArray<float> ArcRemaining;

	// ---- Mask splat blobs (pooled, cap 6 concurrent) ----
	UPROPERTY() TArray<TObjectPtr<UImage>> BlobImages;
	TArray<float> BlobAge;      // < 0 = inactive
	TArray<float> BlobBaseSize; // px

	// ---- Elim confirm text ----
	UPROPERTY() TObjectPtr<UTextBlock> ElimText;
	float ElimTextRemaining = 0.f;
	bool bElimTextNamePending = false;

	// ---- Damage vignette flash ----
	UPROPERTY() TObjectPtr<UImage> DamageFlash;
	float DamageFlashRemaining = 0.f;

	TWeakObjectPtr<ACombatForgeCharacter> BoundPawn;
	TWeakObjectPtr<UPFWeaponComponent> BoundWeapon;
	TWeakObjectPtr<UPFHealthComponent> BoundHealth;
	TWeakObjectPtr<ACombatForgeGameState> BoundGameState;

	static constexpr int32 ArcPoolSize = 6;
	static constexpr int32 BlobPoolSize = 6;
	static constexpr float HitmarkerDuration = 0.22f;
	static constexpr float ArcDuration = 0.85f;
	static constexpr float ArcRadiusPx = 150.f;
	static constexpr float BlobFadePhase = 1.f;
	static constexpr float BlobLifeSeconds = 6.f;
	static constexpr float ElimTextDuration = 1.15f;
	static constexpr float DamageFlashDuration = 0.22f;
};
