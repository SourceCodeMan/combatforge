// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/CombatForgeTypes.h"
#include "PFBuildComponent.generated.h"

class APFBuildGrid;
class ACombatForgePlayerState;
class UEnhancedInputComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPFInputConfig;
class UStaticMesh;
class UStaticMeshComponent;
struct FHitResult;
struct FInputActionValue;

/**
 * Per-character build handling (contract §3.5): IMC_Build input, equip/wheel state, the
 * camera-projected ghost preview, place/delete server RPCs (grid cells, never transforms), and
 * turbo-build. Active only during BuildPhase; the PC swaps input contexts, the component and the
 * server both re-check the phase.
 */
UCLASS(ClassGroup=(CombatForge), meta=(BlueprintSpawnableComponent))
class COMBATFORGE_API UPFBuildComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFBuildComponent();

	/** Called by ACombatForgeCharacter::SetupPlayerInputComponent (§3.3) — binds ALL IMC_Build actions. */
	void BindInput(UEnhancedInputComponent* EIC, const UPFInputConfig* Cfg);

	// ---- Equip state (client-local) ----
	void EquipTool(EPFBuildTool Tool);   // also the wheel-selection entry point (RootHUD wires it)
	EPFBuildTool GetEquippedTool() const { return EquippedTool; }
	/** The tool Dir steps away in the mouse-wheel cycle order (Dir>0 = next / scroll up, Dir<0 = previous). */
	EPFBuildTool CycleNeighbor(int32 Dir) const;

	// ---- RPC surface (binding) ----
	UFUNCTION(Server, Reliable) void ServerPlacePiece(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot);
	UFUNCTION(Server, Reliable) void ServerDeletePiece(uint16 PieceId);
	UFUNCTION(Client, Reliable) void ClientPlaceDenied(EPFDenyReason Reason);

	// ---- UI subscription points ----
	FPFOnEquippedToolChanged  OnEquippedToolChangedEvent;
	FPFOnPlaceDenied          OnPlaceDeniedEvent;
	FPFOnBuildWheelRequested  OnBuildWheelRequestedEvent;   // HOLD Q: press=true (open), release=false (commit hovered)
	/** Digit hotkey (0-9 = sector 1-10) pressed while the wheel is open → RootHUD commits that sector. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnBuildWheelDigit, int32);
	FPFOnBuildWheelDigit      OnBuildWheelDigitEvent;

	/** Called by RootHUD when the wheel closes via digit / Esc / phase change (not via Q). */
	void NotifyBuildWheelClosed();

	bool IsBuildWheelOpen() const { return bWheelOpenSent; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- Input handlers (IMC_Build) ----
	void OnPlaceStarted();
	void OnPlaceReleased();
	void OnDeleteToolPressed();
	void OnRotatePressed();
	void OnCyclePiece(const FInputActionValue& Value);
	void OnEquipWall();
	void OnEquipFloor();
	void OnEquipRamp();
	void OnEquipRoof();
	void OnWheelPressed();
	void OnWheelReleased();
	// One UFUNCTION-free handler per digit key (BindAction takes no payload); all funnel into
	// HandleWheelDigit, which no-ops unless the wheel is open.
	void OnWheelDigit0(); void OnWheelDigit1(); void OnWheelDigit2(); void OnWheelDigit3();
	void OnWheelDigit4(); void OnWheelDigit5(); void OnWheelDigit6(); void OnWheelDigit7();
	void OnWheelDigit8(); void OnWheelDigit9();
	void HandleWheelDigit(int32 Digit);

private:
	/** Last snapped slot sent by turbo (dedup: place on slot change OR every 0.15 s — 03 §4). */
	struct FPFSentSlot
	{
		uint8 Type = 255;
		int16 X = 0;
		int16 Y = 0;
		int16 Z = 0;
		uint8 Rot = 0;
		bool  bValid = false;
	};

	static constexpr float TurboIntervalSec   = 0.15f;    // same-slot turbo cadence
	static constexpr float MinSendIntervalSec = 0.125f;   // client self-cap 8 RPC/s
	static constexpr float DenyFlashSec       = 0.2f;     // ghost red flash on server denial

	APFBuildGrid* GetGrid() const;
	ACombatForgePlayerState* GetOwnerPlayerState() const;

	void EnsureGhost();
	void SetGhostVisible(bool bVisible);
	void SetGhostMeshForType(EPFPieceType Type);
	void ApplyGhostTint(const FLinearColor& Color);
	/** One-way door: small front-side wedge so facing is obvious under a translucent ghost. */
	void UpdateOneWayFacingCue(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot, bool bShow);
	UStaticMesh* MeshForType(EPFPieceType Type) const;

	/** Quantize the anchor point per piece type (03 §4). Returns record-space grid ints. */
	bool ComputeSnappedSlot(EPFPieceType Type, const FVector& AnchorP, float CamYawDeg,
	                        int16& OutX, int16& OutY, int16& OutZ, uint8& OutRot) const;

	void UpdatePlacementGhostAndTurbo(const FVector& CamLoc, const FRotator& CamRot,
	                                  bool bTraceHit, const FHitResult& Hit);
	void UpdateDeleteToolAndTurbo(bool bTraceHit, const FHitResult& Hit);

	/** Place-and-eject (03 §4): server depenetrates overlapped pawns UPWARD onto the piece top. */
	void EjectOverlappedPawns(const FBox& PieceBox) const;

	// Engine assets (CDO-time finders — D10).
	UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY() TObjectPtr<UStaticMesh> ConeMesh;
	UPROPERTY() TObjectPtr<UMaterialInterface> ShapeMaterial;
	/** Translucent unlit parent for placement ghosts (alpha actually works; BasicShape is opaque). */
	UPROPERTY() TObjectPtr<UMaterialInterface> GhostMaterial;

	// Ghost — owning-client only, translucent validity tint, NoCollision, never replicated.
	UPROPERTY() TObjectPtr<UStaticMeshComponent> GhostMesh;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GhostMID;
	/** Front-face arrow for one-way door ghosts (same translucent stack). */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> GhostFacingMesh;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GhostFacingMID;

	EPFBuildTool EquippedTool  = EPFBuildTool::Wall;
	uint8  RampRotOffset = 0;   // resets after placement and on tool switch
	uint8  PropRotOffset = 0;   // persists (03 §2)
	int32  CurrentGhostMeshType = -1;

	bool   bPlaceHeld = false;
	bool   bWheelOpenSent = false;   // true while the build wheel UI is open
	double LastSendTime = -100.0;
	double DenyFlashUntil = 0.0;
	FPFSentSlot LastSentSlot;
	uint16 LastDeleteSentId = 0;

	mutable TWeakObjectPtr<APFBuildGrid> CachedGrid;
	TWeakObjectPtr<UEnhancedInputComponent> LastBoundInput;
};
