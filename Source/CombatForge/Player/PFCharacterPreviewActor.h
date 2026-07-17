// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Player/PFCharacterCustomization.h"   // FPFCharacterConfig
#include "Combat/PFWeaponCatalog.h"            // FPFWeaponConfig
#include "PFCharacterPreviewActor.generated.h"

class USceneComponent;
class USkeletalMeshComponent;
class UStaticMeshComponent;
class USceneCaptureComponent2D;
class UPointLightComponent;
class UTextureRenderTarget2D;
class USkeletalMesh;
class UAnimSequence;

/**
 * Off-screen "photo studio" that renders the modular Bandit character to a render target for the
 * CHARACTER / LOADOUT tabs. Assembled from the same PFChar + PFWeapon registries the in-game pawn uses,
 * so the preview matches what spawns — including the selected gun in the right hand. Spawned far from
 * the arena; a fixed SceneCapture looks at a turntable the user can drag. Nothing here touches gameplay.
 */
UCLASS()
class COMBATFORGE_API APFCharacterPreviewActor : public AActor
{
	GENERATED_BODY()

public:
	APFCharacterPreviewActor();

	virtual void Tick(float DeltaSeconds) override;

	/** Mount base + per-slot parts from a config (mirrors the pawn's ApplyCharacterConfig). */
	void ApplyConfig(const FPFCharacterConfig& Config);

	/** Put the selected weapon mesh in the preview character's hand (mirrors pawn AttachWeaponToHand). */
	void ApplyWeapon(const FPFWeaponConfig& Config);

	/** Enable/disable per-frame capture (off when the tab isn't showing). */
	void SetPreviewActive(bool bActive);

	/** Rotate the turntable by a delta (degrees) — driven by click-drag on the tab. */
	void AddYaw(float DeltaDeg);

	/** The render target the UMG image samples (valid after BeginPlay). */
	UTextureRenderTarget2D* GetRenderTarget() const { return RenderTarget; }

protected:
	virtual void BeginPlay() override;

private:
	void EnsureRenderTarget();
	void AttachPreviewWeapon();

	UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;
	UPROPERTY() TObjectPtr<USceneComponent> Turntable;          // spun each tick; the mesh hangs off it
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> BaseMesh;    // SKM_Body: carries skeleton + idle anim
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> BaseComps;   // fixed base skin (head/legs)
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> SlotComps;   // one per customization slot
	UPROPERTY() TObjectPtr<UStaticMeshComponent> WeaponMeshComp; // selected gun in the right hand
	UPROPERTY() TObjectPtr<USceneCaptureComponent2D> Capture;   // fixed; renders only this actor
	UPROPERTY() TObjectPtr<UPointLightComponent> KeyLight;
	UPROPERTY() TObjectPtr<UPointLightComponent> FillLight;
	UPROPERTY() TObjectPtr<UPointLightComponent> RimLight;
	UPROPERTY() TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	UPROPERTY() TObjectPtr<USkeletalMesh> BodyMeshAsset;
	UPROPERTY() TObjectPtr<UAnimSequence> IdleAnimAsset;

	bool bPreviewActive = false;
	bool bBodyMounted = false;
	float SpinYaw = 0.f;
	FName CachedWeaponBone = NAME_None;
};
