// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/CombatForgeTypes.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "PFBuildGrid.generated.h"

class ACombatForgePlayerState;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
struct FHitResult;

/** FastArray wrapper — lives here, only the grid uses it directly (contract §3.5, B8). */
USTRUCT()
struct COMBATFORGE_API FPFBuildPieceArray : public FFastArraySerializer
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
struct COMBATFORGE_API FPFPlacementQuery
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
class COMBATFORGE_API APFBuildGrid : public AActor
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
	EPFDenyReason TryPlacePiece(ACombatForgePlayerState* Placer, const FPFPlacementQuery& Q, uint16& OutPieceId);

	/**
	 * Server-only delete: exists? same team? phase Build? Refund goes to the ORIGINAL builder
	 * (T19) via GameState roster lookup; removal is MarkArrayDirty'd.
	 */
	EPFDenyReason TryDeletePiece(ACombatForgePlayerState* Requester, uint16 PieceId);

	/** Build→Combat: rejects all further mutation (belt+braces with the phase gate). */
	void FreezeBuild();

	/** Lobby→Build (rematch): wipes array + ISMs + occupancy, unfreezes, resets piece ids. */
	void ClearAll();

	/**
	 * Server-only: inject pre-built pieces (community-map load) into the live grid — re-mints each
	 * PieceId, dirties the FastArray so it replicates, and mirrors the ISM/occupancy locally. Bypasses
	 * budget/rate/plot checks by design (the caller has already chosen legal, remapped coordinates).
	 */
	void ServerInjectPieces(const TArray<FPFBuildPieceRec>& InPieces);

	/** Fingerprint / serialization input. */
	const TArray<FPFBuildPieceRec>& GetPieces() const { return Pieces.Items; }

	/** Vertical bases / height cap for the map currently in play (Warehouse 4/1200, Yard 7/2100). */
	int32 ActiveLevels() const;
	int32 ActiveHeightCapUU() const;

	/** ISM / special-actor hit → piece record, for the client delete-tool highlight + bomb plant. */
	bool FindPieceByHit(const FHitResult& Hit, uint16& OutPieceId, FPFBuildPieceRec& OutRec) const;

	/** Special piece runtime actor (window/door/trap) lookup by piece id. */
	class APFBuildPieceActor* FindSpecialPiece(uint16 PieceId) const;

	// ---- Demolition bomb support (breach a walled-off path; live-grid only, never touches the saved arena) ----
	/** Server/const lookup of a live piece by id (no HitResult). */
	bool FindPieceById(uint16 PieceId, FPFBuildPieceRec& OutRec) const;
	/** Authority-only: remove a piece from the LIVE grid for the rest of the match. Unlike TryDeletePiece this
	 *  has NO phase/team/refund gate (it runs during frozen combat and mints no budget) and writes no file —
	 *  the arena JSON was fingerprinted at Build→Combat, so the piece returns next match / on rebuild. */
	void ServerRemovePieceForMatch(uint16 PieceId);
	/** One bomb per piece: returns false if already armed. */
	bool TrySetPieceBomb(uint16 PieceId);
	void ClearPieceBomb(uint16 PieceId);
	bool IsPieceBombed(uint16 PieceId) const { return BombedPieceIds.Contains(PieceId); }

private:
	friend struct FPFBuildPieceArray;

	struct FPFRateWindow
	{
		double WindowStart = 0.0;
		int32  Count = 0;
	};

	/** Swap prop ISMs to warehouse meshes + apply the cohesion palette. Idempotent; called from
	 *  BeginPlay and lazily from AddPieceLocal so client-side FastArray rebuilds that precede
	 *  BeginPlay don't render prop pieces on the GCube placeholder (giant unskinned cube). */
	void EnsurePieceVisualsApplied();
	/** Latches only on a COMPLETE load — a partial one must be retried, not frozen in. */
	bool bPieceVisualsReady = false;

	/** Re-stamp placed prop instances after a mesh swap (fallback transforms don't fit real props). */
	void RefreshPropInstanceTransforms();
	/** Poll EnsurePieceVisualsApplied until the warehouse content resolves (or we give up). */
	void StartPieceVisualsRetry();
	void TickPieceVisualsRetry();
	FTimerHandle PieceVisualsRetryHandle;
	int32 PieceVisualsRetryCount = 0;
	/** Set once the retry budget is spent. StartPieceVisualsRetry guarded only on IsTimerActive, and
	 *  after give-up the timer is inactive — so every subsequent AddPieceLocal armed a fresh 30-retry
	 *  cycle and re-printed the "GAVE UP" warning. Give up once, stay given up. */
	bool bPieceVisualsGaveUp = false;

	/** Add/remove the local mirror of a record: ISM instance + occupancy (server AND client path). */
	void AddPieceLocal(const FPFBuildPieceRec& Rec);
	void RemovePieceLocal(const FPFBuildPieceRec& Rec);
	void RegisterOccupancy(const FPFBuildPieceRec& Rec);
	void UnregisterOccupancy(const FPFBuildPieceRec& Rec);
	/** Server-only: spawn replicated special piece actor (window/door/trap). */
	void SpawnSpecialPieceActor(const FPFBuildPieceRec& Rec);
	void DestroySpecialPieceActor(uint16 PieceId);
	/**
	 * Local (server+client): solid under-ramp steps leave a crouch-height tunnel under the plank.
	 * Standing capsules (~176uu) cannot crawl deep under; crouch (~116uu) can go further in.
	 */
	void SpawnRampUnderfill(const FPFBuildPieceRec& Rec);
	void DestroyRampUnderfill(uint16 PieceId);

	/** Anchor rule (anti-sky-spam, 03 §4): touches terrain or any structural piece; floaters legal. */
	bool HasAnchor(const FPFPlacementQuery& Q, const FBox& Bounds) const;

	static int32 ISMCIndexFor(EPFPieceType Type, uint8 Team);
	static FIntVector WallEdgeKey(int32 Cx, int32 Cy, int32 Level, uint8 EdgeNE);
	/** Grid rows for the map currently in play (GameState->ArenaMap → map def; Warehouse 10, Yard 20). */
	int32 ActiveCellsY() const;

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

	TMap<uint8, FPFRateWindow> RateWindows;        // roster index → placements this second (server)
	TMap<uint8, FPFRateWindow> DeleteRateWindows;  // roster index → deletes this second (server; issue #12 BD3)

	// Server-only: match-stable builder identity per piece. FPFBuildPieceRec::OwnerIdx is a raw
	// roster index the GameMode may recycle to a later joiner; a refund must never leak to that
	// newcomer, so the refund is gated on the ORIGINAL builder's PlayerState still matching.
	TMap<uint16, TWeakObjectPtr<ACombatForgePlayerState>> BuilderByPieceId;

	// Pieces with a live demolition bomb attached (server-only; enforces one bomb per piece atomically).
	TSet<uint16> BombedPieceIds;

	// Special structural pieces (window / door / trap) — server-spawned, replicate to clients.
	UPROPERTY() TMap<uint16, TObjectPtr<class APFBuildPieceActor>> SpecialPieces;
	// Ramp underside fill (local per machine — mirrors FastArray adds; not a separate replicate).
	UPROPERTY() TMap<uint16, TObjectPtr<AActor>> RampUnderfills;
};
