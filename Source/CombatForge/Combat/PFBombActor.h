// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFBombActor.generated.h"

class APFBuildGrid;
class ACombatForgePlayerState;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * Demolition bomb (anti-griefing breach): planted on a structural build piece during Combat with a 15 s fuse.
 * An ENEMY of the planter can defuse by holding F for a continuous 8 s. On detonation the ONE targeted piece
 * is removed from the LIVE grid for the rest of the match (APFBuildGrid::ServerRemovePieceForMatch) — the
 * saved arena is never touched, so the piece returns next match. Server-authoritative throughout: plant and
 * defuse route through the interacting PAWN's Server RPCs (the barrel pattern — this actor is GameMode-owned,
 * so a client RPC issued directly on it would be dropped).
 */
UCLASS()
class COMBATFORGE_API APFBombActor : public AActor
{
	GENERATED_BODY()

public:
	APFBombActor();

	/** Authority: attach to the target piece and start the 15 s fuse. */
	void ServerArm(APFBuildGrid* Grid, uint16 PieceId, const FVector& WorldLoc,
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

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void ServerDetonate();
	void ServerDefused();
	UFUNCTION() void OnRep_Detonated();
	/** Boom cosmetics on THIS machine (host directly, clients via OnRep) — reuses the frag audio. */
	void PlayDetonationLocal();
	void UpdateLabel();

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	/** Floating countdown / defuse-progress label, tinted to the planter's team color. */
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> CountdownText;

	// Replicated state (clients render the countdown + defuse progress from these).
	UPROPERTY(Replicated) uint8 PlanterTeam = 255;
	UPROPERTY(Replicated) float DetonateServerTime = 0.f;   // GameState server-time of the boom
	UPROPERTY(Replicated) float DefuseAccumSeconds = 0.f;
	UPROPERTY(ReplicatedUsing=OnRep_Detonated) bool bDetonated = false;

	// Server-only.
	bool bArmed = false;
	uint16 TargetPieceId = 0;
	TWeakObjectPtr<APFBuildGrid> GridWeak;
	TWeakObjectPtr<ACombatForgePlayerState> PlanterPS;
	TWeakObjectPtr<APawn> DefuserWeak;
	bool bDefuserHeld = false;
	FTimerHandle FuseTimer;
};
