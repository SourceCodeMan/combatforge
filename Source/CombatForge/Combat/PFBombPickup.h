// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFBombPickup.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * Floating demolition charge at mid-field. Combat-only: walk up and press F to claim.
 * Once taken, the pickup despawns and the GameMode respawns another after RespawnDelaySeconds
 * (unlimited supply — players do NOT spawn with a bomb).
 */
UCLASS()
class COMBATFORGE_API APFBombPickup : public AActor
{
	GENERATED_BODY()

public:
	APFBombPickup();

	/** Authority: place at world location and enable pickup. */
	void ServerActivateAt(const FVector& WorldLoc);

	bool IsAvailable() const { return bAvailable; }

	/** Server-authority claim (validates phase/range/already-carrying). Called from the pawn's Server RPC. */
	void AuthorityInteract(APawn* Interactor);

	static constexpr float InteractRangeUU = 240.f;
	static constexpr float HoverHeightUU   = 90.f;   // float above the floor pad

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutReplicatedProps) const override;

	UFUNCTION() void OnRep_Available();

	void SoftLoadMesh();
	void ApplyAvailableVisuals();
	void UpdateLabelFacing();

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> InteractSphere;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> SignText;

	UPROPERTY(ReplicatedUsing=OnRep_Available) bool bAvailable = true;

	float SpinYaw = 0.f;
	float BobPhase = 0.f;
};
