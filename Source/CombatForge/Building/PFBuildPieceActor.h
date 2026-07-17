// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/CombatForgeTypes.h"
#include "PFBuildPieceActor.generated.h"

class UStaticMeshComponent;
class UBoxComponent;
class USceneComponent;

/**
 * Runtime actor for special structural pieces that can't live in a single ISM instance:
 *  - WallWindow: frame with large shoot/see-through opening
 *  - WallDoor: two-way swinging door (F to toggle)
 *  - WallDoorOneWay: single-thickness trick door — looks like a normal door until the first
 *    open→close from the front, then a flush seal plate fills the aperture (wall look, no
 *    double thickness). Always openable only from the front.
 *  - FloorTrap: drops when an ENEMY of the placing team stands on it, then reseals
 *
 * Placement still lives in APFBuildGrid's FastArray; this actor is pure runtime collision/visuals/state.
 */
UCLASS()
class COMBATFORGE_API APFBuildPieceActor : public AActor
{
	GENERATED_BODY()

public:
	APFBuildPieceActor();

	/** Called on server+client when the piece is mirrored into the world. */
	void InitFromRecord(const FPFBuildPieceRec& Rec);

	uint16 GetPieceId() const { return PieceId; }
	EPFPieceType GetPieceType() const { return PieceType; }
	bool IsOpen() const { return bOpen; }

	/** Server: toggle two-way / one-way door (one-way only from front side). */
	void AuthorityTryToggleDoor(APawn* User);

	/** True if User is within interact range of a usable door face. */
	bool CanUserToggleDoor(const APawn* User) const;

	/** World position used for door range checks (leaf center, not cell corner). */
	FVector GetDoorInteractLocation() const { return DoorLeafClosedCenter(); }

	static constexpr float DoorInteractRangeUU = 300.f;
	/** How long a door stays open after F before auto-closing. */
	static constexpr float DoorOpenSeconds = 3.f;
	/** How long the trap stays open after an enemy trips it. */
	static constexpr float TrapOpenSeconds = 3.5f;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Open();
	UFUNCTION() void OnRep_OneWaySealed();
	/** Clients: rebuild mesh when PieceId/type arrive after BeginPlay. */
	UFUNCTION() void OnRep_PieceMeta();

	void RebuildGeometry();
	void ApplyOpenState();
	void ApplyCollisionPreset(UPrimitiveComponent* Comp, bool bBlock);
	void EnsureGeometryBuilt();

	UStaticMeshComponent* AddCubePart(const FName& Name, const FVector& WorldCenter,
		const FVector& WorldExtent, const FRotator& WorldRot, UMaterialInterface* Mat, bool bBlock);

	/** Wall frame basis: origin = cell min corner world, Rot 0=N / 1=E. */
	void BuildWallFrameParts(bool bWithDoorOpening, bool bWithWindowOpening);
	void BuildDoorLeaf();
	/** Flush aperture fill (same plane/thickness as the door leaf) — shown only after first seal. */
	void BuildOneWaySealPlate();
	void BuildTrapFloor();
	void TickTrap(float DeltaSeconds);
	void TickDoorAnim(float DeltaSeconds);
	/** Server: count down open doors and close them after DoorOpenSeconds. */
	void TickDoorAutoClose(float DeltaSeconds);
	void AuthorityCloseDoor();

	/** Front outward normal of the wall edge (+Y for N, +X for E). */
	FVector WallFrontNormal() const;
	FVector DoorLeafClosedCenter() const;
	FRotator DoorLeafClosedRotation() const;
	FRotator DoorLeafOpenRotation() const;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;

	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> SolidParts;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> DoorLeaf;
	/** Flush wall fill in the door aperture after first open→close (not a second offset wall). */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> OneWaySealPlate;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> TrapPlate;
	UPROPERTY() TObjectPtr<UBoxComponent> TrapTrigger;

	UPROPERTY(ReplicatedUsing=OnRep_PieceMeta) uint16 PieceId = 0;
	UPROPERTY(ReplicatedUsing=OnRep_PieceMeta) EPFPieceType PieceType = EPFPieceType::WallWindow;
	UPROPERTY(ReplicatedUsing=OnRep_PieceMeta) int16 GridX = 0;
	UPROPERTY(ReplicatedUsing=OnRep_PieceMeta) int16 GridY = 0;
	UPROPERTY(ReplicatedUsing=OnRep_PieceMeta) int16 GridZ = 0;
	UPROPERTY(ReplicatedUsing=OnRep_PieceMeta) uint8 GridRot = 0;
	UPROPERTY(Replicated) uint8 TeamId = 0;
	UPROPERTY(ReplicatedUsing=OnRep_Open) bool bOpen = false;
	/** One-way only: set true after the first front-side open→close; enables the flush seal plate. */
	UPROPERTY(ReplicatedUsing=OnRep_OneWaySealed) bool bOneWaySealed = false;

	float DoorYawAlpha = 0.f;     // 0 closed .. 1 open (visual)
	float DoorOpenRemaining = 0.f; // server auto-close countdown while open
	float TrapOpenRemaining = 0.f;

	UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> FrameMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> DoorMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> TrapMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PaneMID; // translucent window (no collision)
};
