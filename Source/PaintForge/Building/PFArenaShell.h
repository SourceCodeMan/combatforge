// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/PaintForgeTypes.h"
#include "PFArenaShell.generated.h"

class AGameStateBase;
class APaintForgeGameState;
class UBoxComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Static world geometry (T9, T23, T29), constructor-built identically everywhere from engine
 * primitives: field floor slab (6400×4000×30, top at Z=0), 4 perimeter walls (h=1200),
 * team-tinted spawn-strip floor tiles, the see-through midline (gray posts every 400 uu + floor
 * stripe + an invisible full-height blocking volume active during BuildPhase only), and the
 * south warm-up pen (20×20 m at Y=-3000) with 12 dummy slots.
 *
 * Art pass: per-role materials + Cosmetic warehouse dressing (ceiling, trusses, dock bays,
 * wall ribs) — all NoCollision and above HeightCap so they never block build/trace/play.
 * Collision, scales, and spawn transforms for functional parts are unchanged.
 * Lights/fog live in UPFLightingSubsystem. Replicated for existence only.
 */
UCLASS()
class PAINTFORGE_API APFArenaShell : public AActor
{
	GENERATED_BODY()

public:
	APFArenaShell();

	/**
	 * BuildPhase: invisible blocker (Pawn + Paintball) ON. GameMode (server) drives; clients
	 * mirror via the GameState phase delegate so their predicted movement blocks identically.
	 */
	void SetMidlineBarrierActive(bool bActive);

	// ---- Spawn transform providers (GameMode consumes; deterministic, index-stable) ----
	FTransform GetTeamSpawnTransform(uint8 TeamSide, int32 SlotIdx) const;   // TeamSide = physical side 0/1
	FTransform GetBuildStartTransform(uint8 Team, int32 SlotIdx) const;      // own-plot placement at Build start
	FTransform GetWarmupSpawnTransform(int32 SlotIdx) const;                 // pen player spawns
	FTransform GetWarmupDummyTransform(int32 SlotIdx) const;                 // pen dummy slots (T29)

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	enum class EPFShellCollision : uint8
	{
		Solid,            // block Pawn / Visibility / Paintball
		SolidBuildable,   // Solid + block BuildTrace (field floor: ghost snap target — §4.6)
		Cosmetic          // NoCollision (strips, dressing, hazard paint)
	};

	UStaticMeshComponent* MakeShapePart(const FString& Name, const FVector& Center,
	                                    const FVector& Scale, EPFShellCollision Mode,
	                                    UMaterialInterface* Material,
	                                    const FRotator& RelRot = FRotator::ZeroRotator,
	                                    bool bCastShadow = false);

	/** Ceiling, trusses, dock bays, wall ribs — all Cosmetic, Z ≥ 1400, off play volume. */
	void BuildWarehouseDressing();

	void ApplyTint(UStaticMeshComponent* Comp, const FLinearColor& Color);
	void BindToGameState(APaintForgeGameState* GS);
	void OnGameStateSet(AGameStateBase* NewGameState);
	void HandlePhaseChanged(EPFMatchPhase NewPhase);

	UPROPERTY() TObjectPtr<USceneComponent> ShellRoot;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> FieldFloor;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> PerimeterWalls;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> SpawnStrips;      // [0]=side A (west), [1]=side B (east)
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> MidlinePosts;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> MidlineStripe;
	UPROPERTY() TObjectPtr<UBoxComponent> MidlineBarrier;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> PenFloor;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> PenWalls;

	// Cosmetic warehouse dressing (existence-only; never solid).
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> DressingParts;

	UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
	// Per-role arena art materials (soft CDO load; BasicShapeMaterial fallback).
	UPROPERTY() TObjectPtr<UMaterialInterface> FloorMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> WallMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> MetalMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> MarkMaterial;   // Color-driven spawn / hazard lines
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> TintMIDs;

	FDelegateHandle GameStateSetHandle;
};
