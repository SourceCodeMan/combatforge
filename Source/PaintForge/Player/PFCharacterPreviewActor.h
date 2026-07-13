// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Player/PFCharacterCustomization.h"   // FPFCharacterConfig
#include "PFCharacterPreviewActor.generated.h"

class USceneComponent;
class USkeletalMeshComponent;
class USceneCaptureComponent2D;
class UPointLightComponent;
class UTextureRenderTarget2D;
class USkeletalMesh;
class UAnimSequence;

/**
 * Off-screen "photo studio" that renders the modular Bandit character to a render target for the
 * CHARACTER customization tab. Assembled from the same PFChar registry the in-game pawn uses, so the
 * preview always matches what spawns. Spawned far from the arena; a fixed SceneCapture looks at a
 * turntable that spins the character while the tab is showing. Nothing here touches gameplay.
 */
UCLASS()
class PAINTFORGE_API APFCharacterPreviewActor : public AActor
{
	GENERATED_BODY()

public:
	APFCharacterPreviewActor();

	virtual void Tick(float DeltaSeconds) override;

	/** Mount base + per-slot parts from a config (mirrors the pawn's ApplyCharacterConfig). */
	void ApplyConfig(const FPFCharacterConfig& Config);

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

	UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;
	UPROPERTY() TObjectPtr<USceneComponent> Turntable;          // spun each tick; the mesh hangs off it
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> BaseMesh;    // SKM_Body: carries skeleton + idle anim
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> BaseComps;   // fixed base skin (head/legs)
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> SlotComps;   // one per customization slot
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
};
