// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFFlagActor.generated.h"

class APaintForgePlayerState;
class UMaterialInstanceDynamic;
class USphereComponent;
class UStaticMeshComponent;

/**
 * CTF flag (Objectives/). Replicated visual + overlap volume; carrier/capture truth lives on
 * PlayerState / GameMode. Positions come from PFObjectiveLayout (PFGrid), not ArenaShell.
 */
UCLASS()
class PAINTFORGE_API APFFlagActor : public AActor
{
	GENERATED_BODY()

public:
	APFFlagActor();

	/** Server: stamp home team + home location, place at home, color mesh. */
	void ServerInit(uint8 InOwnerTeam, const FVector& InHomeLocation);

	uint8 GetOwnerTeam() const { return OwnerTeam; }
	bool IsAtHome() const { return bAtHome; }
	bool IsCarried() const { return bCarried; }
	APaintForgePlayerState* GetCarrier() const { return CarrierPS.Get(); }
	FVector GetHomeLocation() const { return HomeLocation; }

	/** Server: attach visual to carrier pawn; hide at base. */
	void ServerGiveTo(APaintForgePlayerState* Carrier);
	/** Server: detach, clear carrier, teleport to home. */
	void ServerReturnHome();
	/** Server: drop at world location (not carried, not home). */
	void ServerDropAt(const FVector& WorldLoc);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnPickupOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnRep_VisualState();

private:
	void ApplyTeamColor();
	void ApplyVisualState();

	UPROPERTY(VisibleAnywhere, Category="PF|Flag") TObjectPtr<UStaticMeshComponent> FlagMesh;
	UPROPERTY(VisibleAnywhere, Category="PF|Flag") TObjectPtr<USphereComponent> PickupSphere;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> FlagMID;

	UPROPERTY(ReplicatedUsing=OnRep_VisualState) uint8 OwnerTeam = 0;
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) bool bAtHome = true;
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) bool bCarried = false;
	UPROPERTY(Replicated) FVector_NetQuantize HomeLocation = FVector::ZeroVector;

	/** Server-only carrier; clients see attach via follow-tick when bCarried. */
	TWeakObjectPtr<APaintForgePlayerState> CarrierPS;
};
