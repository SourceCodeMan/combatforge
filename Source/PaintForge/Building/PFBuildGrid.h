// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/PaintForgeTypes.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "PFBuildGrid.generated.h"

class APaintForgePlayerState;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
struct FHitResult;

/** FastArray wrapper — lives here, only the grid uses it directly (contract §3.5, B8). */
USTRUCT()
struct PAINTFORGE_API FPFBuildPieceArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY() TArray<FPFBuildPieceRec> Items;
	UPROPERTY(NotReplicated) TObjectPtr<class APFBuildGrid> OwnerGrid;   // set server+client in ctor/BeginPlay

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FPFBuildPieceRec, FPFBuildPieceArray>(Items, DeltaParms, *this);
	}

	// Client mirror hooks — visuals/occupancy come ONLY from these (§5.11):
	void PostReplicatedAdd(const TArrayView<int32>& AddedIndices, int32 FinalSize);
	void PreReplicatedRemove(const TArrayView<int32>& RemovedIndices, int32 FinalSize);
	void PostReplicatedChange(const TArrayView<int32>& ChangedIndices, int32 FinalSize); // unused in v1 (immutable)
};

template<>
struct TStructOpsTypeTraits<FPFBuildPieceArray> : public TStructOpsTypeTraitsBase2<FPFBuildPieceArray>
{
	enum { WithNetDeltaSerializer = true };
};

/** Placement query — one struct so client ghost and server validation share ONE predicate (03 §4). */
USTRUCT()
struct PAINTFORGE_API FPFPlacementQuery
{
	GENERATED_BODY()
	UPROPERTY() EPFPieceType Type = EPFPieceType::Wall;
	UPROPERTY() int16 X = 0;   // FPFBuildPieceRec semantics
	UPROPERTY() int16 Y = 0;
	UPROPERTY() int16 Z = 0;
	UPROPERTY() uint8 Rot = 0;
	UPROPERTY() uint8 Team = 0;
};

/**
 * The single replicated arena container (B8): FastArray of compact piece records + 14 ISMCs
 * (7 piece types × 2 team tints — T8) + occupancy maps + the shared placement predicate.
 * Server mutates; clients mirror from FastArray callbacks.
 */
UCLASS()
class PAINTFORGE_API APFBuildGrid : public AActor
{
	GENERATED_BODY()

public:
	APFBuildGrid();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated) FPFBuildPieceArray Pieces;

	/**
	 * Shared validation — client ghost tint AND server authority run the same function.
	 * Checks: phase, plot ownership (incl. spawn-column/neutral exclusion), height cap, slot
	 * occupancy (structural), AABB overlap (props), anchor rule. Does NOT check budget/rate
	 * (server-only, in TryPlacePiece).
	 */
	EPFDenyReason QueryPlacement(const FPFPlacementQuery& Q) const;

	/**
	 * Server-only mutation (called from UPFBuildComponent server RPCs). QueryPlacement + rate cap
	 * (10/s/player) + budget spend + PieceId assignment + MarkItemDirty. Never trusts client
	 * Team/Owner — reads them from Placer.
	 */
	EPFDenyReason TryPlacePiece(APaintForgePlayerState* Placer, const FPFPlacementQuery& Q, uint16& OutPieceId);

	/**
	 * Server-only delete: exists? same team? phase Build? Refund goes to the ORIGINAL builder
	 * (T19) via GameState roster lookup; removal is MarkArrayDirty'd.
	 */
	EPFDenyReason TryDeletePiece(APaintForgePlayerState* Requester, uint16 PieceId);

	/** Build→Combat: rejects all further mutation (belt+braces with the phase gate). */
	void FreezeBuild();

	/** Lobby→Build (rematch): wipes array + ISMs + occupancy, unfreezes, resets piece ids. */
	void ClearAll();

	/** Fingerprint / serialization input. */
	const TArray<FPFBuildPieceRec>& GetPieces() const { return Pieces.Items; }

	/** ISM hit → piece record, for the client delete-tool highlight. */
	bool FindPieceByHit(const FHitResult& Hit, uint16& OutPieceId, FPFBuildPieceRec& OutRec) const;

private:
	friend struct FPFBuildPieceArray;

	struct FPFRateWindow
	{
		double WindowStart = 0.0;
		int32  Count = 0;
	};

	/** Add/remove the local mirror of a record: ISM instance + occupancy (server AND client path). */
	void AddPieceLocal(const FPFBuildPieceRec& Rec);
	void RemovePieceLocal(const FPFBuildPieceRec& Rec);
	void RegisterOccupancy(const FPFBuildPieceRec& Rec);
	void UnregisterOccupancy(const FPFBuildPieceRec& Rec);

	/** Anchor rule (anti-sky-spam, 03 §4): touches terrain or any structural piece; floaters legal. */
	bool HasAnchor(const FPFPlacementQuery& Q, const FBox& Bounds) const;

	static int32 ISMCIndexFor(EPFPieceType Type, uint8 Team);
	static FIntVector WallEdgeKey(int32 Cx, int32 Cy, int32 Level, uint8 EdgeNE);

	UPROPERTY() TObjectPtr<USceneComponent> GridRoot;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> PieceISMCs[14];
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> TeamMIDs[14];
	UPROPERTY() TObjectPtr<UMaterialInterface> ShapeMaterial;

	bool   bBuildFrozen = false;
	uint16 NextPieceId  = 0;   // monotonic per match, never reused

	// Occupancy mirrors (rebuilt on clients from FastArray callbacks; the server remains the judge).
	TMap<FIntVector, uint16> FloorSlots;     // (cellX, cellY, level)
	TMap<FIntVector, uint16> InclineSlots;   // (cellX, cellY, level) — ramp XOR roof
	TMap<FIntVector, uint16> WallEdges;      // (cellX, cellY, level*2 + edge) — canonical N/E
	TMap<uint16, FBox> StructuralBounds;     // PieceId → world AABB (anchor + prop-overlap tests)
	TMap<uint16, FBox> PropBounds;

	// ISM bookkeeping: survives ISM swap-removal.
	TMap<int32, uint16> InstanceToPiece[14]; // per-ISMC instance index → PieceId
	TMap<uint16, int32> PieceToInstance;     // PieceId → instance index within its ISMC

	TMap<uint8, FPFRateWindow> RateWindows;  // roster index → placements this second (server)
};
