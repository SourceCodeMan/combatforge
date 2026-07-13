// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFAmmoBarrel.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * Interactable ammo refill (Combat only). Soft-loads a warehouse barrel mesh when present.
 * Server grants full mag + full reserve. Permanent station for the whole combat phase — refill as often as
 * you like. A floating "AMMO" sign is always visible so it reads as a resupply point from across the arena.
 */
UCLASS()
class PAINTFORGE_API APFAmmoBarrel : public AActor
{
	GENERATED_BODY()

public:
	APFAmmoBarrel();

	/** Authority: place at world location and re-enable interact. */
	void ServerActivateAt(const FVector& WorldLoc);

	bool IsAvailable() const { return bAvailable; }

	/** Local player pressed interact while overlapping — routes to server. */
	void LocalRequestInteract();

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnOverlapBegin(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	UFUNCTION() void OnOverlapEnd(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

	UFUNCTION() void OnRep_Available();

	UFUNCTION(Server, Reliable) void ServerInteract(APawn* Interactor);

	void SoftLoadMesh();
	void UpdatePromptVisibility();
	void ApplyAvailableVisuals();
	/** Fixed (non-billboarding) label facing toward the near team's half, set once at spawn. */
	void OrientLabels();

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> InteractSphere;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> PromptText;
	/** Always-visible floating beacon ("AMMO") so the station is obvious from a distance. */
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> SignText;

	UPROPERTY(ReplicatedUsing=OnRep_Available) bool bAvailable = true;

	TWeakObjectPtr<APawn> LocalOverlappingPawn;
};
