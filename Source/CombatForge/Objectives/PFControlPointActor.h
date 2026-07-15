// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PFControlPointActor.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * Domination / Hardpoint control volume (Objectives/). Server evaluates occupancy;
 * GameMode owns scoring ticks and Hardpoint rotation. Positions from PFObjectiveLayout.
 *
 * Domination (CoD model): all three zones live simultaneously. This actor also carries the replicated
 * CAPTURE state (who is capturing, how far along, contested) so every HUD can draw the progress bar and
 * the zone visuals can pulse while a capture is running. The capture MATH runs in the GameMode's 1 Hz
 * scoring tick (ServerTickCapture); this actor just stores/replicates the result.
 */
UCLASS()
class COMBATFORGE_API APFControlPointActor : public AActor
{
	GENERATED_BODY()

public:
	APFControlPointActor();

	/** Server: slot index + world location + active flag. */
	void ServerInit(int32 InPointIndex, const FVector& WorldLoc, bool bInitiallyActive);

	int32 GetPointIndex() const { return PointIndex; }
	uint8 GetControllingTeam() const { return ControllingTeam; }
	bool IsPointActive() const { return bActive; }

	// Capture state (replicated; valid in Domination — Hardpoint leaves these idle).
	uint8 GetCapturingTeam() const { return CapturingTeam; }
	float GetCaptureProgress01() const { return CaptureProgress01; }
	bool IsContested() const { return bContested; }

	/** Server: Hardpoint on/off; Dom keeps always-on. */
	void ServerSetActive(bool bNewActive);
	/** Server: set owner (0/1 or 255 neutral). */
	void ServerSetControllingTeam(uint8 Team);

	/**
	 * Server, Domination, called at 1 Hz by the GameMode with this zone's occupancy: advances the CoD-style
	 * capture chain. Rules (research: CoD Wiki + Activision MW2019/MWIII/BO6 guides, adapted per Tom's spec):
	 *  - Sole team T inside: progress at min(N,3) x rate. Opposing stored progress unwinds first ("take it
	 *    back"), then T's own capture builds.
	 *  - Enemy-OWNED zone is two-stage: neutralize (owner -> 255), then capture — 2x total time.
	 *  - Contested (both teams inside): progress FROZEN.
	 *  - Empty: progress PERSISTS (Tom's explicit spec — diverges from CoD's slow decay on purpose).
	 * Returns true if ownership changed this tick.
	 */
	bool ServerTickCapture(int32 CountA, int32 CountB, float DeltaSeconds, float CaptureSeconds);

	/**
	 * Server: count living teamed pawns currently overlapping the capture sphere.
	 * OutA/OutB = players per team. Returns sole-team controller if uncontested, else 255.
	 * OutOccupants (optional) collects the counted PlayerStates — the HUD stamp must derive from the SAME
	 * overlap set as the count, or players at the zone edge capture with no capture bar on their screen.
	 */
	uint8 ServerQueryOccupancy(int32& OutA, int32& OutB,
		TArray<class ACombatForgePlayerState*>* OutOccupants = nullptr) const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnRep_VisualState();

private:
	void ApplyVisualState();
	void EnsureObjectiveMaterials();   // soft-resolve the /Game masters once (BeginPlay / first apply)
	FLinearColor CurrentTeamColor() const;

	UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;   // uniform-scale root so the sphere isn't squashed by the pad
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UStaticMeshComponent> PadMesh;
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<USphereComponent> CaptureSphere;
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UStaticMeshComponent> PoleMesh;   // flag pole
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UStaticMeshComponent> FlagMesh;   // flag (team-colored)
	// Floating point letter (A/B/C) above the flag — back-to-back pair reads from both spawns without a tick
	// or billboarding (same pattern as the ammo barrel's AMMO sign).
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UTextRenderComponent> LetterFront;
	UPROPERTY(VisibleAnywhere, Category="PF|CP") TObjectPtr<UTextRenderComponent> LetterBack;
	// Zone-boundary pillars: short glow posts ringing the capture radius so the zone edge reads in-world
	// (BO6 marks its larger zones on the ground the same way). Team-colored with the pad.
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> RingPillars;
	// MID masters: MIDs are ALWAYS created from these (never from whatever sits in the mesh slot —
	// CreateAndSetMaterialInstanceDynamic re-parents to the slot's current MID, see 8f631b5).
	UPROPERTY() TObjectPtr<UMaterialInterface> MarkMaterial;           // M_PF_ArenaMark — pad/pillars/flag
	UPROPERTY() TObjectPtr<UMaterialInterface> MetalMaterial;          // M_PF_ArenaMetal — pole
	UPROPERTY() TObjectPtr<UMaterialInterface> FallbackBaseMaterial;   // /Engine BasicShapeMaterial (ctor hard ref)
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PadMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> FlagMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PoleMID;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> PillarMIDs;
	bool bTriedObjectiveMaterials = false;

	UPROPERTY(ReplicatedUsing=OnRep_VisualState) int32 PointIndex = 0;
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) uint8 ControllingTeam = 255; // 255 = neutral
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) bool bActive = true;

	// Domination capture chain (server-written, replicated for HUD/pulse).
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) uint8 CapturingTeam = 255;   // team with stored progress
	UPROPERTY(Replicated) float CaptureProgress01 = 0.f;                      // 0..1 of the CURRENT stage
	UPROPERTY(ReplicatedUsing=OnRep_VisualState) bool bContested = false;

	float PulsePhase = 0.f;   // client-side capture pulse
};
