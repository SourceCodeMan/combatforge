// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/CombatForgeTypes.h"
#include "PFArenaShell.generated.h"

class AGameStateBase;
class ACombatForgeGameState;
class UBoxComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Static world geometry (T9, T23, T29), constructor-built identically everywhere from engine
 * primitives: field floor slab (FieldX×FieldY×30, top at Z=0 — Warehouse 6400×4000), 4 perimeter
 * walls (h=1200), team-tinted spawn-strip floor tiles, the see-through midline (gray posts every
 * 400 uu + floor stripe + an invisible full-height blocking volume active during BuildPhase
 * only), and the south warm-up pen (20×20 m at Y=-3000) with 12 dummy slots.
 *
 * Field dimensions come from an FPFArenaMapDef (task #40): the default ctor builds the Warehouse
 * (byte-identical to the pre-selector arena); subclasses (APFYardShell) delegate the protected
 * ctor with their own def. Map identity travels as ACTOR CLASS — spawning the right class IS the
 * replication mechanism, keeping the "geometry ctor-built identically everywhere" contract.
 * The build GRID is PFGrid's 16×10 on every map; a wider field is an open lane, not more plots.
 *
 * Art pass: per-role materials + Cosmetic warehouse dressing (ceiling, trusses, dock bays,
 * wall ribs) — all NoCollision and above HeightCap so they never block build/trace/play.
 * Collision, scales, and spawn transforms for functional parts are unchanged.
 * Lights/fog live in UPFLightingSubsystem. Replicated for existence only.
 */
UCLASS()
class COMBATFORGE_API APFArenaShell : public AActor
{
	GENERATED_BODY()

public:
	APFArenaShell();

	// ---- Map/field queries (GameMode barrel corners, bot field-centre, stream gate) ----
	const FPFArenaMapDef& GetMapDef() const { return MapDef; }
	FVector2D GetFieldSize() const { return FVector2D(FieldX, FieldY); }
	FVector GetFieldCenter() const { return FVector(FieldX * 0.5f, FieldY * 0.5f, 0.f); }

	/**
	 * BuildPhase: invisible blocker (Pawn + Paintball) ON. GameMode (server) drives; clients
	 * mirror via the GameState phase delegate so their predicted movement blocks identically.
	 */
	void SetMidlineBarrierActive(bool bActive);

	/**
	 * When the Industrial_Warehouse map is streamed as the environment, hide cube floor/walls/
	 * dressing (keep collision + spawn strips + midline paint for gameplay).
	 */
	void SetMapBackdropActive(bool bActive);
	bool IsMapBackdropActive() const { return bMapBackdropActive; }

	// ---- Spawn transform providers (GameMode consumes; deterministic, index-stable) ----
	FTransform GetTeamSpawnTransform(uint8 TeamSide, int32 SlotIdx) const;   // TeamSide = physical side 0/1
	FTransform GetBuildStartTransform(uint8 Team, int32 SlotIdx) const;      // own-plot placement at Build start
	FTransform GetWarmupSpawnTransform(int32 SlotIdx) const;                 // pen player spawns
	FTransform GetWarmupDummyTransform(int32 SlotIdx) const;                 // pen dummy slots (T29)
	/** FFA combat: random point on the play field (margin from walls). Salt diversifies picks. */
	FTransform GetRandomFieldSpawnTransform(int32 Salt) const;

protected:
	/** Map-parameterized build (task #40): subclasses delegate with their own def (see APFYardShell). */
	explicit APFArenaShell(const FPFArenaMapDef& InDef);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Protected (not private) below: APFYardShell reuses the part helpers / materials / dims to
	// build its facade scenery in its own ctor with the exact same construction rules.
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

	/** Same as MakeShapePart but with an arbitrary static mesh (warehouse props). */
	UStaticMeshComponent* MakeMeshPart(const FString& Name, UStaticMesh* Mesh, const FVector& Center,
	                                   const FVector& Scale, EPFShellCollision Mode,
	                                   UMaterialInterface* Material,
	                                   const FRotator& RelRot = FRotator::ZeroRotator,
	                                   bool bCastShadow = false);

	/** Ceiling, trusses, dock bays, wall ribs — all Cosmetic, Z ≥ 1400, off play volume. */
	void BuildWarehouseDressing();

	/**
	 * Soft-load Scene_Warehouse Megascans (BeginPlay only — never CDO) and drape real
	 * structural props outside/above the 64×40 m play rectangle. NoCollision. Falls back
	 * cleanly when the pack is missing; hides cube stand-ins when real meshes spawn.
	 */
	void BuildWarehouseBackdropDrape();

	/** Soft-load warehouse meshes into Prop* slots (idempotent). Returns true if any mesh loaded. */
	bool SoftLoadWarehouseMeshes();

	/**
	 * Runtime mesh part (BeginPlay / post-ctor). CreateDefaultSubobject is ctor-only;
	 * backdrop drapes must use NewObject + RegisterComponent.
	 */
	UStaticMeshComponent* MakeRuntimeMeshPart(const FString& Name, UStaticMesh* Mesh,
	                                          const FVector& Center, const FVector& Scale,
	                                          EPFShellCollision Mode, UMaterialInterface* Material,
	                                          const FRotator& RelRot = FRotator::ZeroRotator,
	                                          bool bCastShadow = false);

	/** Place mesh with bottom on a Z plane (World XY + GroundZ). Scale fitted to TargetSize if non-zero. */
	UStaticMeshComponent* PlaceBackdropFitted(const FString& Name, UStaticMesh* Mesh,
	                                          const FVector& WorldXY, float GroundZ,
	                                          const FVector& TargetSize, const FRotator& YawRot,
	                                          bool bCastShadow = true);

	void HideCubeDressingByPrefix(const TCHAR* Prefix);

	void ApplyTint(UStaticMeshComponent* Comp, const FLinearColor& Color);
	/** Rebind floor/wall/metal cube shell to the shared warehouse cohesion palette (BeginPlay). */
	void ApplyCohesivePalette();
	void BindToGameState(ACombatForgeGameState* GS);
	void OnGameStateSet(AGameStateBase* NewGameState);
	void HandlePhaseChanged(EPFMatchPhase NewPhase);

	// ---- Per-map field parameters (ctor init list, before any geometry is built) ----
	// Same names the old file-scope constexprs had, so every geometry consumer reads identically;
	// Warehouse values are numerically identical to the former constants (byte-identical arena).
	FPFArenaMapDef MapDef;
	float FieldX = 6400.f;              // MapDef.FieldX
	float FieldY = 4000.f;              // MapDef.FieldY
	float PenCenterX = 3200.f;          // FieldX * 0.5 (pen stays south at Y=-3000 on every map)
	float TeamSpawnSpacingY = 4000.f / PFGrid::SpawnPointsPerTeam;
	float PenSlotStartX = 0.f;          // derived from PenCenterX in the ctor init list
	// Vertical extents follow the MAP's build cap, not the Warehouse constant: the Yard builds to 2100 uu,
	// so a 1800-uu midline barrier / open-field bound was hoppable from high Yard decks (issue #12 BD1).
	// Warehouse values are numerically identical to the former file-scope constants (1800 / 1350).
	float PerimeterH = static_cast<float>(PFGrid::HeightCapUU) + 600.f;   // MapDef.HeightCapUU + 600
	float EscapeLidZ = static_cast<float>(PFGrid::HeightCapUU) + 150.f;   // MapDef.HeightCapUU + 150

	UPROPERTY() TObjectPtr<USceneComponent> ShellRoot;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> FieldFloor;
	/** Invisible ceiling lid above HeightCap — stops jumping out after building high. */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> EscapeLid;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> PerimeterWalls;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> SpawnStrips;      // [0]=side A (west), [1]=side B (east)
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> MidlinePosts;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> MidlineStripe;
	UPROPERTY() TObjectPtr<UBoxComponent> MidlineBarrier;
	// Tinted-glass midline SCREEN (Tom 2026-07-23): dark near-opaque at build start so teams can't peek
	// across while building, fading to fully clear over the first MidWallFadeSeconds. Cosmetic only — the
	// crossing block is MidlineBarrier above; this is what you SEE. Fully transparent + hidden in combat.
	UPROPERTY() TObjectPtr<UStaticMeshComponent> MidWallScreen;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> MidWallMID;
	UPROPERTY() TObjectPtr<UMaterialInterface> MidWallBaseMaterial;   // /Engine .../M_SimpleUnlitTranslucent
	UPROPERTY() TObjectPtr<UStaticMeshComponent> PenFloor;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> PenWalls;

	// Cosmetic warehouse dressing (existence-only; never solid).
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> DressingParts;

	UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
	// Soft-loaded warehouse meshes (null if Scene_Warehouse not present). BeginPlay only.
	UPROPERTY() TObjectPtr<UStaticMesh> PropBarrelMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropBoxMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropCeilingLightMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropLadderMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropBeamMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropBulkheadMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropShelfMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropPalletMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> PropCabinetMesh;
	// Per-role arena art materials (soft CDO load; BasicShapeMaterial fallback).
	UPROPERTY() TObjectPtr<UMaterialInterface> FloorMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> WallMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> MetalMaterial;
	UPROPERTY() TObjectPtr<UMaterialInterface> MarkMaterial;   // Color-driven spawn / hazard lines
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> TintMIDs;

	// ---- Midline tint-screen fade config (build-phase visual) ----
	// Opaque -> clear over the first 2 minutes of the build phase (Tom: "over the first two minutes...
	// gradually change opacity towards being clear. From minute 2 to minute 30, completely clear").
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float MidWallFadeSeconds  = 120.f;
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") float MidWallStartOpacity = 0.96f;   // near-opaque tinted glass
	UPROPERTY(EditDefaultsOnly, Category="PF|Match") FLinearColor MidWallTint  = FLinearColor(0.012f, 0.017f, 0.03f, 1.f);
	double MidWallFadeStartTime = -1.0;   // world seconds when the current build phase began (-1 = idle/hidden)

	void SetupMidWallScreen();            // ctor: build the screen mesh over the midline
	void BeginMidWallFade();              // build-phase entered: opaque now, start the clock
	void UpdateMidWallFade();             // per-tick lerp toward clear; hides + stops when done
	void ApplyMidWallOpacity(float Alpha01);

	FDelegateHandle GameStateSetHandle;
	bool bMapBackdropActive = false;
	bool bWarehouseMeshesLoaded = false;
	bool bWarehouseDrapeBuilt = false;

public:
	virtual void Tick(float DeltaSeconds) override;
	/** Dev preview (pf.MidWall): optionally override the look, then re-arm the opaque->clear fade now. */
	void PreviewMidWallFade(float StartOpacity01, float FadeSeconds);
	/** Dev (pf.MidWallMat): swap the screen's base material live (test a glass material), rebuild the MID,
	 *  and re-arm the fade so you can see whether its opacity actually drives. */
	void SetMidWallMaterial(UMaterialInterface* Mat);
};
