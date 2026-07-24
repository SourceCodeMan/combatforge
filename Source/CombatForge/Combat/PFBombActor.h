// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "Core/CombatForgeTypes.h"
#include "PFBombActor.generated.h"

class APFBuildGrid;
class ACombatForgePlayerState;
class UPFWeaponComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * Demolition bomb (anti-griefing breach): planted on a structural build piece during Combat with a 15 s fuse.
 * An ENEMY of the planter can defuse by holding F for a continuous 8 s. On detonation the ONE targeted piece
 * is removed from the LIVE grid for the rest of the match (APFBuildGrid::ServerRemovePieceForMatch) — the
 * saved arena is never touched, so the piece returns next match — AND a dual-sided full-sphere BB burst
 * (FragBBCount pellets from BOTH faces of the piece) paints anyone nearby on either side. Remaining cover
 * still blocks individual pellets; teammates are immune (B12). Server-authoritative throughout: plant and
 * defuse route through the interacting PAWN's Server RPCs (the barrel pattern — this actor is GameMode-owned,
 * so a client RPC issued directly on it would be dropped).
 */
UCLASS()
class COMBATFORGE_API APFBombActor : public AActor
{
	GENERATED_BODY()

public:
	APFBombActor();

	/**
	 * Authority: attach to the target piece and start the 15 s fuse.
	 * Orient/offset from PieceRec + Planter world pos so the charge sits on the planted face
	 * (wall: planter's side only; floor/ramp: matches surface pitch/yaw).
	 */
	void ServerArm(APFBuildGrid* Grid, uint16 PieceId, const FPFBuildPieceRec& PieceRec,
	               const FVector& PieceCenter, const FVector& PlanterWorldLoc,
	               uint8 InPlanterTeam, ACombatForgePlayerState* InPlanterPS);

	/** Server: a pawn began/stopped holding F on this bomb. The bomb's own Tick re-validates range/alive/team
	 *  every frame and accumulates the 8 s — releasing, dying, or stepping away resets progress to zero. */
	void ServerSetDefuser(APawn* Defuser, bool bActive);

	bool IsArmed() const { return bArmed && !bDetonated; }
	uint8 GetPlanterTeam() const { return PlanterTeam; }
	ACombatForgePlayerState* GetPlanterPS() const { return PlanterPS.Get(); }

	static constexpr float FuseSeconds       = 15.f;
	static constexpr float DefuseHoldSeconds = 8.f;
	static constexpr float DefuseRangeUU     = 260.f;   // hold-F reach (slightly over barrel interact range)
	/** Radial BB count on detonation — same pipeline as the frag grenade (90), scaled up for a breach
	 *  charge. P2-CB4: was 1000, which cost ~13 frames of 80 SpawnActor each on the listen host for a
	 *  fantasy ("breach + paint everyone nearby") that ApplyProximityPaint already delivers up close.
	 *  120 keeps a real LOS spray beyond the paint radius at ~1/8 the actor cost. */
	static constexpr int32 FragBBCount       = 120;
	/** Spawn this many authoritative BBs per frame (spreads the cost so detonation never hitches). */
	static constexpr int32 BurstBatchSize    = 40;
	/** Seconds between batches (~1 frame at 60 Hz). */
	static constexpr float BurstBatchInterval = 0.016f;
	/** Distance past the piece center along the face normal for each side's spawn origin (uu). Puts
	 *  the burst into BOTH rooms instead of half the pellets dying into the wall/floor plane. */
	static constexpr float BurstSideOffsetUU = 40.f;
	/** Hard radius for the guaranteed proximity paint (uu) — standing on the charge always hurts. */
	static constexpr float ProximityPaintRadiusUU = 280.f;
	/** Client cosmetic tracers only (pool is 64) — never loop FragBBCount times on remotes. */
	static constexpr int32 CosmeticTracerCount = 64;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutReplicatedProps) const override;

	void ServerDetonate();
	void ServerDefused();
	UFUNCTION() void OnRep_Detonated();
	/** Boom cosmetics on THIS machine (host directly, clients via OnRep) — frag audio + cosmetic BB spray. */
	void PlayDetonationLocal();
	/** Kick off the multi-frame BB spray (first batch now, rest on timer). */
	void BeginFragBurst(const FVector& Center);
	/** One batch of authoritative BBs; timer drives until FragBBCount is reached. */
	void SpawnFragBurstBatch();
	/** Non-host clients: capped cosmetic tracers (not 1000 spawns). */
	void SpawnCosmeticFragBurst(const FVector& Center, const FVector& FaceAxis, uint32 Seed);
	/** Guaranteed close-range paint: any non-teammate (and the planter) inside radius takes a hit. */
	void ApplyProximityPaint(const FVector& Center);
	/** Unit normal through the piece face: wall ±N/E, floor/roof ±Z, ramp ±sideways (burst only). */
	static FVector FaceAxisForPiece(EPFPieceType Type, uint8 Rot);
	/** World pose: on the planted surface, facing outward toward the planter (walls) or surface-up (floor/ramp). */
	static void ComputePlantPose(const FPFBuildPieceRec& Rec, const FVector& PieceCenter,
	                             const FVector& PlanterWorldLoc, FVector& OutLoc, FRotator& OutRot);
	void ApplyPlantedPose();
	void UpdateLabel();
	UFUNCTION() void OnRep_PlantedPose();

	/** Soft-load Bandits grenade mesh (red preferred) so the planted charge isn't a graybox cylinder. */
	void SoftLoadMesh();

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	/** Floating countdown / defuse-progress label, tinted to the planter's team color. */
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> CountdownText;

	// Replicated state (clients render the countdown + defuse progress from these).
	UPROPERTY(Replicated) uint8 PlanterTeam = 255;
	UPROPERTY(Replicated) float DetonateServerTime = 0.f;   // GameState server-time of the boom
	UPROPERTY(Replicated) float DefuseAccumSeconds = 0.f;
	UPROPERTY(ReplicatedUsing=OnRep_Detonated) bool bDetonated = false;
	/** REPLICATED — the client-side F-press pre-scan reads IsArmed(); a server-only flag left joined
	 *  clients unable to ever begin a defuse (review wf_e923820a). */
	UPROPERTY(Replicated) bool bArmed = false;
	/** Shared frag-spread seed so remote cosmetic tracers match the server BB directions. */
	UPROPERTY(Replicated) uint32 BurstSeed = 0;
	/** Face normal of the bombed piece — dual-side burst origins sit at Center ± Axis * Offset. */
	UPROPERTY(Replicated) FVector_NetQuantizeNormal BurstAxis = FVector(0.f, 0.f, 1.f);
	/** Planted transform (no continuous spin). Replicated so clients match wall-side / ramp pitch. */
	UPROPERTY(ReplicatedUsing=OnRep_PlantedPose) FVector_NetQuantize100 PlantedLocation = FVector::ZeroVector;
	UPROPERTY(ReplicatedUsing=OnRep_PlantedPose) FRotator PlantedRotation = FRotator::ZeroRotator;
	UPROPERTY(Replicated) bool bPlantedPoseValid = false;

	// Server-only.
	uint16 TargetPieceId = 0;
	TWeakObjectPtr<APFBuildGrid> GridWeak;
	TWeakObjectPtr<ACombatForgePlayerState> PlanterPS;
	TWeakObjectPtr<APawn> DefuserWeak;
	bool bDefuserHeld = false;
	FTimerHandle FuseTimer;
	// Fuse pause while someone is ACTIVELY defusing (server-only): the countdown freezes so a defuser can't
	// blow up mid-defuse (bomb 3s left + 8s defuse used to detonate at defuse-5s). Not replicated — the frozen
	// HUD rides the replicated DetonateServerTime, which we hold constant during the pause.
	bool  bFusePaused = false;
	float FusePausedRemaining = 0.f;

	// Deferred BB spray (server): avoid SpawnActor×1000 on the detonation frame.
	FVector BurstCenter = FVector::ZeroVector;
	int32 BurstNextIndex = 0;
	TWeakObjectPtr<UPFWeaponComponent> BurstWeaponWeak;
	TWeakObjectPtr<APawn> BurstPlanterPawnWeak;
	FTimerHandle BurstTimer;
};
