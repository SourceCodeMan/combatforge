// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/PaintForgeTypes.h"
#include "PFCombatFeedbackWidget.generated.h"

class APaintForgeCharacter;
class APaintForgeGameState;
class UCanvasPanel;
class UImage;
class UPFHealthComponent;
class UPFWeaponComponent;
class UTextBlock;

/**
 * Owning-client combat feedback (contract §3.6, 04 §4):
 *  - Weapon->OnHitConfirmedEvent -> hitmarker (4 ticks, 0.15 s; elim variant
 *    x1.4 team-tinted) + CombatAudio->PlayHitmarker()/PlayElim()
 *  - Health->OnLocalPaintHitTakenEvent -> damage-direction arc (radius 140 px,
 *    0.75 s fade) + mask splats (2-3 blobs, edge-biased, 60-140 px,
 *    0.85 -> 0.35 opacity over 1 s, wiped over 6 s or on round reset, CAP 6)
 *  - "SPLATTED [name]" center text 0.9 s on own elim confirm
 * Pawn wiring is pushed in by UPFRootHUDWidget (BindToPawn) on possession change.
 */
UCLASS()
class PAINTFORGE_API UPFCombatFeedbackWidget : public UUserWidget
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

	TWeakObjectPtr<APaintForgeCharacter> BoundPawn;
	TWeakObjectPtr<UPFWeaponComponent> BoundWeapon;
	TWeakObjectPtr<UPFHealthComponent> BoundHealth;
	TWeakObjectPtr<APaintForgeGameState> BoundGameState;

	static constexpr int32 ArcPoolSize = 6;
	static constexpr int32 BlobPoolSize = 6;   // cap 6 concurrent (04 §4)
	static constexpr float HitmarkerDuration = 0.15f;
	static constexpr float ArcDuration = 0.75f;
	static constexpr float ArcRadiusPx = 140.f;
	static constexpr float BlobFadePhase = 1.f;   // 0.85 -> 0.35 opacity
	static constexpr float BlobLifetime = 6.f;    // full wipe
	static constexpr float ElimTextDuration = 0.9f;
};
