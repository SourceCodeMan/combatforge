// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFControlPointActor.generated.h"

class UMaterialInstanceDynamic;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * Domination / Hardpoint control volume (Objectives/). Server evaluates occupancy;
 * GameMode owns scoring ticks and Hardpoint rotation. Positions from PFObjectiveLayout.
 */
UCLASS()
class PAINTFORGE_API APFControlPointActor : public AActor
{
	GENERATED_BODY()

public:
	APFControlPointActor();

	/** Server: slot index + world location + active flag. */
	void ServerInit(int32 InPointIndex, const FVector& WorldLoc, bool bInitiallyActive);

	int32 GetPointIndex() const { return PointIndex; }
	uint8 GetControllingTeam() const { return ControllingTeam; }
	bool IsPointActive() const { return bActive; }

	/** Server: Hardpoint on/off; Dom keeps always-on. */
	void ServerSetActive(bool bNewActive);
	/** Server: set owner (0/1 or 255 neutral). */
	void ServerSetControllingTeam(uint8 Team);

	/**
	 * Server: count living teamed pawns currently overlapping the capture sphere.
	 * OutA/OutB = players per team. Returns sole-team controller if uncontested, else 255.
	 */
	uint8 ServerQueryOccupancy(int32& OutA, int32& OutB) const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnRep_VisualState();

private:
	void ApplyVisualState();

	UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;   // uniform-scale root so the sphere isn't squashed by the pad
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UStaticMeshComponent> PadMesh;
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<USphereComponent> CaptureSphere;
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UStaticMeshComponent> PoleMesh;   // flag pole
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UStaticMeshComponent> FlagMesh;   // flag (team-colored)
	// Floating point letter (A/B/C) above the flag — back-to-back pair reads from both spawns without a tick
	// or billboarding (same pattern as the ammo barrel's AMMO sign).
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UTextRenderComponent> LetterFront;
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UTextRenderComponent> LetterBack;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PadMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> FlagMID;

	UPROPERTY(ReplicatedUsing=OnRep_VisualState) int32 PointIndex = 0;
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) uint8 ControllingTeam = 255; // 255 = neutral
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) bool bActive = true;
};
