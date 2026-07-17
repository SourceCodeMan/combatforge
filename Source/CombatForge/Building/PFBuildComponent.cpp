// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildComponent.h"

#include "CombatForge.h"
#include "Building/PFBuildGrid.h"
#include "Building/PFBuildPieceVisuals.h"
#include "Building/PFGridMath.h"
#include "Combat/PFCombatAudio.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Input/PFInputConfig.h"
#include "Player/CombatForgeCharacter.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Mouse-wheel / cycle order: classic structural → specials → props → delete.
	constexpr EPFBuildTool GCycleOrder[12] =
	{
		EPFBuildTool::Wall, EPFBuildTool::Floor, EPFBuildTool::Ramp, EPFBuildTool::Roof,
		EPFBuildTool::WallWindow, EPFBuildTool::WallDoor, EPFBuildTool::WallDoorOneWay, EPFBuildTool::FloorTrap,
		EPFBuildTool::PropCan, EPFBuildTool::PropDorito, EPFBuildTool::PropSnake, EPFBuildTool::Delete
	};
}

UPFBuildComponent::UPFBuildComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);   // carries RPCs (§5.2)

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	// Ghost material: prefer the engine BasicShapeMaterial (ISM-safe solid "Color" tint) over M_PF_BuildPiece,
	// which renders as the UE CHECKER on instanced meshes (the "checkered build pieces" bug). A clean tinted
	// ghost also reads better than the concrete master for a placement preview.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMatFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ArtMatFinder(TEXT("/Game/Materials/M_PF_BuildPiece.M_PF_BuildPiece"));
	CubeMesh      = CubeFinder.Object;
	CylinderMesh  = CylinderFinder.Object;
	ConeMesh      = ConeFinder.Object;
	ShapeMaterial = BasicMatFinder.Succeeded() ? BasicMatFinder.Object : ArtMatFinder.Object;
}

void UPFBuildComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GhostMesh)
	{
		GhostMesh->DestroyComponent();
		GhostMesh = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void UPFBuildComponent::BindInput(UEnhancedInputComponent* EIC, const UPFInputConfig* Cfg)
{
	if (!EIC || !Cfg || LastBoundInput.Get() == EIC)
	{
		return;
	}
	LastBoundInput = EIC;

	EIC->BindAction(Cfg->IA_Place, ETriggerEvent::Started, this, &UPFBuildComponent::OnPlaceStarted);
	EIC->BindAction(Cfg->IA_Place, ETriggerEvent::Completed, this, &UPFBuildComponent::OnPlaceReleased);
	EIC->BindAction(Cfg->IA_Place, ETriggerEvent::Canceled, this, &UPFBuildComponent::OnPlaceReleased);

	EIC->BindAction(Cfg->IA_DeleteTool, ETriggerEvent::Started, this, &UPFBuildComponent::OnDeleteToolPressed);
	EIC->BindAction(Cfg->IA_RotatePiece, ETriggerEvent::Started, this, &UPFBuildComponent::OnRotatePressed);
	EIC->BindAction(Cfg->IA_CyclePiece, ETriggerEvent::Triggered, this, &UPFBuildComponent::OnCyclePiece);

	EIC->BindAction(Cfg->IA_EquipWall, ETriggerEvent::Started, this, &UPFBuildComponent::OnEquipWall);
	EIC->BindAction(Cfg->IA_EquipFloor, ETriggerEvent::Started, this, &UPFBuildComponent::OnEquipFloor);
	EIC->BindAction(Cfg->IA_EquipRamp, ETriggerEvent::Started, this, &UPFBuildComponent::OnEquipRamp);
	EIC->BindAction(Cfg->IA_EquipRoof, ETriggerEvent::Started, this, &UPFBuildComponent::OnEquipRoof);

	// Q-tap fires Triggered once on release under the tap threshold.
	EIC->BindAction(Cfg->IA_QuickEquip, ETriggerEvent::Triggered, this, &UPFBuildComponent::OnQuickEquip);

	// Q-hold: the hold trigger enters Triggered when the 0.18 s threshold lands ("open" per T4);
	// Completed on release commits the wheel. Guarded so the wheel opens exactly once per hold.
	EIC->BindAction(Cfg->IA_BuildWheel, ETriggerEvent::Triggered, this, &UPFBuildComponent::OnWheelHoldTriggered);
	EIC->BindAction(Cfg->IA_BuildWheel, ETriggerEvent::Completed, this, &UPFBuildComponent::OnWheelReleased);
	EIC->BindAction(Cfg->IA_BuildWheel, ETriggerEvent::Canceled, this, &UPFBuildComponent::OnWheelReleased);
}

void UPFBuildComponent::OnPlaceStarted()
{
	bPlaceHeld = true;
}

void UPFBuildComponent::OnPlaceReleased()
{
	bPlaceHeld = false;
}

void UPFBuildComponent::OnDeleteToolPressed()
{
	EquipTool(EPFBuildTool::Delete);
}

void UPFBuildComponent::OnRotatePressed()
{
	if (EquippedTool == EPFBuildTool::Ramp)
	{
		RampRotOffset = (RampRotOffset + 1) % 4;
	}
	else if (EquippedTool == EPFBuildTool::PropCan ||
	         EquippedTool == EPFBuildTool::PropDorito ||
	         EquippedTool == EPFBuildTool::PropSnake)
	{
		PropRotOffset = (PropRotOffset + 1) % 4;
	}
	// Wall/Floor/Roof/Delete: no-op (03 §3).
}

void UPFBuildComponent::OnCyclePiece(const FInputActionValue& Value)
{
	const float Axis = Value.Get<float>();
	if (FMath::IsNearlyZero(Axis))
	{
		return;
	}
	constexpr int32 N = UE_ARRAY_COUNT(GCycleOrder);
	int32 Idx = 0;
	for (int32 I = 0; I < N; ++I)
	{
		if (GCycleOrder[I] == EquippedTool)
		{
			Idx = I;
			break;
		}
	}
	Idx = (Idx + (Axis > 0.f ? 1 : (N - 1))) % N;
	EquipTool(GCycleOrder[Idx]);
}

EPFBuildTool UPFBuildComponent::CycleNeighbor(int32 Dir) const
{
	constexpr int32 N = UE_ARRAY_COUNT(GCycleOrder);
	int32 Idx = 0;
	for (int32 I = 0; I < N; ++I)
	{
		if (GCycleOrder[I] == EquippedTool) { Idx = I; break; }
	}
	Idx = (Idx + (Dir > 0 ? 1 : (N - 1))) % N;
	return GCycleOrder[Idx];
}

void UPFBuildComponent::OnEquipWall()  { EquipTool(EPFBuildTool::Wall); }
void UPFBuildComponent::OnEquipFloor() { EquipTool(EPFBuildTool::Floor); }
void UPFBuildComponent::OnEquipRamp()  { EquipTool(EPFBuildTool::Ramp); }
void UPFBuildComponent::OnEquipRoof()  { EquipTool(EPFBuildTool::Roof); }

void UPFBuildComponent::OnQuickEquip()
{
	EquipTool(LastUsedPiece);
}

void UPFBuildComponent::OnWheelHoldTriggered()
{
	if (!bWheelOpenSent)
	{
		bWheelOpenSent = true;
		OnBuildWheelRequestedEvent.Broadcast(true);
	}
}

void UPFBuildComponent::OnWheelReleased()
{
	if (bWheelOpenSent)
	{
		bWheelOpenSent = false;
		OnBuildWheelRequestedEvent.Broadcast(false);
	}
}

void UPFBuildComponent::EquipTool(EPFBuildTool Tool)
{
	if (Tool != EquippedTool)
	{
		RampRotOffset = 0;   // ramp offset resets on switch; prop offset persists (§3.5)
	}
	EquippedTool = Tool;
	if (Tool != EPFBuildTool::Delete)
	{
		LastUsedPiece = Tool;
	}
	OnEquippedToolChangedEvent.Broadcast(EquippedTool);
}

// ---------------------------------------------------------------------------
// Ghost + turbo (owning client, per tick)
// ---------------------------------------------------------------------------

void UPFBuildComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ACharacter* Char = Cast<ACharacter>(GetOwner());
	UWorld* World = GetWorld();
	if (!Char || !World || World->GetNetMode() == NM_DedicatedServer || !Char->IsLocallyControlled())
	{
		return;
	}

	const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>();
	// Play-only matches pass through a REAL (replicated) 0.1s Build phase for the community-arena inject, which
	// used to flash a floating ghost wall at spawn for the first second. Nobody builds in play-only — suppress
	// the ghost entirely there. (FreeForAll is play-only by rule; mirrors the GameMode's play-only predicate.)
	const bool bPlayOnly = GS != nullptr
		&& (GS->BuildMode == EPFBuildMode::PlayOnly || GS->MatchType == EPFMatchType::FreeForAll);
	if (!GS || !GS->IsBuildAllowed() || bPlayOnly)
	{
		SetGhostVisible(false);
		LastSentSlot.bValid = false;
		return;
	}

	APlayerController* PC = Cast<APlayerController>(Char->GetController());
	if (!PC || !PC->PlayerCameraManager)
	{
		SetGhostVisible(false);
		return;
	}

	const FVector  CamLoc = PC->PlayerCameraManager->GetCameraLocation();
	const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();

	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.bTraceComplex = false;
	Params.AddIgnoredActor(Char);
	const FVector TraceEnd = CamLoc + CamRot.Vector() * PFGrid::BuildReachUU;
	const bool bTraceHit = World->LineTraceSingleByChannel(Hit, CamLoc, TraceEnd, PF_ECC_BuildTrace, Params);

	if (EquippedTool == EPFBuildTool::Delete)
	{
		UpdateDeleteToolAndTurbo(bTraceHit, Hit);
	}
	else
	{
		UpdatePlacementGhostAndTurbo(CamLoc, CamRot, bTraceHit, Hit);
	}
}

bool UPFBuildComponent::ComputeSnappedSlot(EPFPieceType Type, const FVector& AnchorP, float CamYawDeg,
                                           int16& OutX, int16& OutY, int16& OutZ, uint8& OutRot) const
{
	// Per-map vertical stack (Warehouse 3 wall stories / Yard 6). Prefers the live grid's map def.
	int32 MapLevels = PFGrid::Levels;
	int32 MapHeightCap = PFGrid::HeightCapUU;
	if (const APFBuildGrid* Grid = GetGrid())
	{
		MapLevels = Grid->ActiveLevels();
		MapHeightCap = Grid->ActiveHeightCapUU();
	}
	else if (const UWorld* World = GetWorld())
	{
		if (const ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
		{
			const FPFArenaMapDef& Def = PFGetArenaMapDef(GS->ArenaMap);
			MapLevels = Def.Levels;
			MapHeightCap = Def.HeightCapUU;
		}
	}

	switch (Type)
	{
	case EPFPieceType::Wall:
	case EPFPieceType::WallWindow:
	case EPFPieceType::WallDoor:
	case EPFPieceType::WallDoorOneWay:
	{
		int32 Cx = 0, Cy = 0, Level = 0;
		uint8 Edge = 0;
		FPFGridMath::SnapWall(AnchorP, Cx, Cy, Level, Edge, MapLevels);
		OutX = static_cast<int16>(Cx * PFGrid::SubPerCell);
		OutY = static_cast<int16>(Cy * PFGrid::SubPerCell);
		OutZ = static_cast<int16>(Level * 3);
		OutRot = Edge;
		return true;
	}
	case EPFPieceType::Floor:
	case EPFPieceType::FloorTrap:
	case EPFPieceType::Ramp:
	case EPFPieceType::Roof:
	{
		const FIntVector Cell = FPFGridMath::WorldToCell(AnchorP, MapLevels);
		OutX = static_cast<int16>(Cell.X * PFGrid::SubPerCell);
		OutY = static_cast<int16>(Cell.Y * PFGrid::SubPerCell);
		OutZ = static_cast<int16>(Cell.Z * 3);
		OutRot = (Type == EPFPieceType::Ramp)
			? FPFGridMath::RampYawFromCamera(CamYawDeg, RampRotOffset)
			: 0;
		return true;
	}
	case EPFPieceType::PropCan:
	case EPFPieceType::PropDorito:
	case EPFPieceType::PropSnake:
	{
		const FIntVector Sub = FPFGridMath::WorldToSubGrid(AnchorP);
		OutX = static_cast<int16>(Sub.X);
		OutY = static_cast<int16>(Sub.Y);
		OutZ = FPFGridMath::SupportTopSubZ(GetWorld(), AnchorP, MapLevels, MapHeightCap);
		OutRot = PropRotOffset;
		return true;
	}
	default:
		return false;
	}
}

void UPFBuildComponent::UpdatePlacementGhostAndTurbo(const FVector& CamLoc, const FRotator& CamRot,
                                                     bool bTraceHit, const FHitResult& Hit)
{
	// Anchor point: hit + normal×8 bias, else camera + fwd×800 (03 §4).
	const FVector AnchorP = bTraceHit
		? Hit.ImpactPoint + Hit.ImpactNormal * 8.f
		: CamLoc + CamRot.Vector() * 800.f;

	if (EquippedTool == EPFBuildTool::Delete)
	{
		SetGhostVisible(false);
		return;
	}
	const EPFPieceType Type = static_cast<EPFPieceType>(EquippedTool);
	if (Type >= EPFPieceType::MAX_Count)
	{
		SetGhostVisible(false);
		return;
	}
	int16 X = 0, Y = 0, Z = 0;
	uint8 Rot = 0;
	if (!ComputeSnappedSlot(Type, AnchorP, CamRot.Yaw, X, Y, Z, Rot))
	{
		SetGhostVisible(false);
		return;
	}

	APFBuildGrid* Grid = GetGrid();
	ACombatForgePlayerState* PS = GetOwnerPlayerState();
	if (!Grid || !PS)
	{
		SetGhostVisible(false);
		return;
	}

	FPFPlacementQuery Q;
	Q.Type = Type;
	Q.X = X;
	Q.Y = Y;
	Q.Z = Z;
	Q.Rot = Rot;
	Q.Team = PS->TeamId;
	const EPFDenyReason Reason = Grid->QueryPlacement(Q);

	// Invalid ghost still renders at the snapped slot — players learn the grid by seeing it (03 §4).
	EnsureGhost();
	SetGhostMeshForType(Type);
	if (GhostMesh)
	{
		GhostMesh->SetWorldTransform(PFBuildPieceVisuals::PieceWorldTransform(Type, X, Y, Z, Rot));
	}
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const bool bDenyFlash = Now < DenyFlashUntil;
	if (GhostMID)
	{
		GhostMID->SetVectorParameterValue(TEXT("Color"),
			(bDenyFlash || Reason != EPFDenyReason::None) ? PFColors::GhostInvalid : PFColors::GhostValid);
	}
	SetGhostVisible(true);

	// Turbo (03 §4): place on snapped-slot change OR every 0.15 s; client self-cap 8 RPC/s.
	if (bPlaceHeld && Reason == EPFDenyReason::None)
	{
		const bool bSlotChanged = !LastSentSlot.bValid
			|| LastSentSlot.Type != static_cast<uint8>(Type)
			|| LastSentSlot.X != X || LastSentSlot.Y != Y || LastSentSlot.Z != Z
			|| LastSentSlot.Rot != Rot;
		if (Now - LastSendTime >= MinSendIntervalSec &&
			(bSlotChanged || Now - LastSendTime >= TurboIntervalSec))
		{
			ServerPlacePiece(Type, X, Y, Z, Rot);
			LastSendTime = Now;
			LastSentSlot = { static_cast<uint8>(Type), X, Y, Z, Rot, true };
			if (Type == EPFPieceType::Ramp)
			{
				RampRotOffset = 0;   // ramp offset resets after each placement (Fortnite behavior)
			}
			// Local place thunk — server may still deny; deny tone overlays if so.
			if (const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwner()))
			{
				if (Char->IsLocallyControlled())
				{
					if (UPFCombatAudio* Audio = Char->GetCombatAudio())
					{
						Audio->PlayPlace();
					}
				}
			}
		}
	}
}

void UPFBuildComponent::UpdateDeleteToolAndTurbo(bool bTraceHit, const FHitResult& Hit)
{
	APFBuildGrid* Grid = GetGrid();
	ACombatForgePlayerState* PS = GetOwnerPlayerState();

	uint16 TargetId = 0;
	FPFBuildPieceRec Rec;
	const bool bHaveTarget = Grid && PS && bTraceHit
		&& Grid->FindPieceByHit(Hit, TargetId, Rec)
		&& Rec.Team == PS->TeamId;   // your team's pieces only, ever (B6)

	if (!bHaveTarget)
	{
		SetGhostVisible(false);
		return;
	}

	// Orange highlight: ghost mesh over the aimed piece, slightly inflated (T7 colors).
	EnsureGhost();
	SetGhostMeshForType(Rec.Type);
	if (GhostMesh)
	{
		FTransform T = PFBuildPieceVisuals::PieceWorldTransform(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot);
		T.SetScale3D(T.GetScale3D() * 1.03f);
		GhostMesh->SetWorldTransform(T);
	}
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (GhostMID)
	{
		GhostMID->SetVectorParameterValue(TEXT("Color"),
			(Now < DenyFlashUntil) ? PFColors::GhostInvalid : PFColors::DeleteHighlight);
	}
	SetGhostVisible(true);

	// Turbo-delete, same cadence as turbo-build (03 §4).
	if (bPlaceHeld)
	{
		if (Now - LastSendTime >= MinSendIntervalSec &&
			(TargetId != LastDeleteSentId || Now - LastSendTime >= TurboIntervalSec))
		{
			ServerDeletePiece(TargetId);
			LastDeleteSentId = TargetId;
			LastSendTime = Now;
			if (const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwner()))
			{
				if (Char->IsLocallyControlled())
				{
					if (UPFCombatAudio* Audio = Char->GetCombatAudio())
					{
						Audio->PlayDelete();
					}
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// RPCs
// ---------------------------------------------------------------------------

void UPFBuildComponent::ServerPlacePiece_Implementation(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot)
{
	APawn* Pawn = Cast<APawn>(GetOwner());
	ACombatForgePlayerState* PS = Pawn ? Pawn->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	APFBuildGrid* Grid = GetGrid();
	if (!PS || !Grid)
	{
		return;
	}

	FPFPlacementQuery Q;
	Q.Type = Type;
	Q.X = X;
	Q.Y = Y;
	Q.Z = Z;
	Q.Rot = Rot;
	Q.Team = PS->TeamId;   // server truth; TryPlacePiece re-reads it from the PlayerState anyway

	uint16 NewPieceId = 0;
	const EPFDenyReason Reason = Grid->TryPlacePiece(PS, Q, NewPieceId);
	if (Reason == EPFDenyReason::None)
	{
		// Place-and-eject: depenetrate any overlapped pawn upward onto the new piece top (03 §4).
		FBox PieceBox(ForceInit);
		if (FPFGridMath::PieceAABB(Type, X, Y, Z, Rot, PieceBox))
		{
			EjectOverlappedPawns(PieceBox);
		}
	}
	else if (Reason != EPFDenyReason::RateLimited)   // rate-limited excess is silently dropped (03 §4)
	{
		ClientPlaceDenied(Reason);
	}
}

void UPFBuildComponent::ServerDeletePiece_Implementation(uint16 PieceId)
{
	APawn* Pawn = Cast<APawn>(GetOwner());
	ACombatForgePlayerState* PS = Pawn ? Pawn->GetPlayerState<ACombatForgePlayerState>() : nullptr;
	APFBuildGrid* Grid = GetGrid();
	if (!PS || !Grid)
	{
		return;
	}
	const EPFDenyReason Reason = Grid->TryDeletePiece(PS, PieceId);
	if (Reason != EPFDenyReason::None)
	{
		ClientPlaceDenied(Reason);
	}
}

void UPFBuildComponent::ClientPlaceDenied_Implementation(EPFDenyReason Reason)
{
	const UWorld* World = GetWorld();
	DenyFlashUntil = (World ? World->GetTimeSeconds() : 0.0) + DenyFlashSec;
	OnPlaceDeniedEvent.Broadcast(Reason);

	if (const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwner()))
	{
		if (UPFCombatAudio* Audio = Char->GetCombatAudio())
		{
			Audio->PlayDenied();
		}
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

APFBuildGrid* UPFBuildComponent::GetGrid() const
{
	if (CachedGrid.IsValid())
	{
		return CachedGrid.Get();
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<APFBuildGrid> It(World); It; ++It)
		{
			CachedGrid = *It;
			return *It;
		}
	}
	return nullptr;
}

ACombatForgePlayerState* UPFBuildComponent::GetOwnerPlayerState() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	return Pawn ? Pawn->GetPlayerState<ACombatForgePlayerState>() : nullptr;
}

void UPFBuildComponent::EnsureGhost()
{
	if (GhostMesh || !GetOwner())
	{
		return;
	}
	GhostMesh = NewObject<UStaticMeshComponent>(GetOwner());
	GhostMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMesh->SetCastShadow(false);
	GhostMesh->SetCanEverAffectNavigation(false);
	GhostMesh->SetVisibility(false);
	GhostMesh->RegisterComponent();

	if (ShapeMaterial)
	{
		GhostMID = UMaterialInstanceDynamic::Create(ShapeMaterial, this);
		GhostMID->SetVectorParameterValue(TEXT("Color"), PFColors::GhostValid);
		GhostMesh->SetMaterial(0, GhostMID);
	}
}

void UPFBuildComponent::SetGhostVisible(bool bVisible)
{
	if (GhostMesh && GhostMesh->IsVisible() != bVisible)
	{
		GhostMesh->SetVisibility(bVisible);
	}
}

void UPFBuildComponent::SetGhostMeshForType(EPFPieceType Type)
{
	if (!GhostMesh || CurrentGhostMeshType == static_cast<int32>(Type))
	{
		return;
	}
	GhostMesh->SetStaticMesh(MeshForType(Type));
	if (GhostMID)
	{
		// Structural ghost: same warehouse surface as placed pieces (validity Color still wins).
		if (!PFIsProp(Type))
		{
			PFBuildPieceVisuals::ApplyStructuralSurface(GhostMID, Type);
		}
		// Warehouse props can have many material slots — tint every one for validity color.
		const int32 NumMats = FMath::Max(1, GhostMesh->GetNumMaterials());
		for (int32 i = 0; i < NumMats; ++i)
		{
			GhostMesh->SetMaterial(i, GhostMID);
		}
	}
	CurrentGhostMeshType = static_cast<int32>(Type);
}

UStaticMesh* UPFBuildComponent::MeshForType(EPFPieceType Type) const
{
	// Shared catalog: warehouse barrel/crate/boxes for props; basic shapes for structural.
	if (UStaticMesh* Shared = PFBuildPieceVisuals::MeshForType(Type))
	{
		return Shared;
	}
	switch (Type)
	{
	case EPFPieceType::Roof:
		return ConeMesh;
	case EPFPieceType::PropCan:
		return CylinderMesh;
	default:
		return CubeMesh;
	}
}

void UPFBuildComponent::EjectOverlappedPawns(const FBox& PieceBox) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		ACharacter* Pawn = *It;
		const UCapsuleComponent* Capsule = Pawn ? Pawn->GetCapsuleComponent() : nullptr;
		if (!Capsule)
		{
			continue;
		}
		const FVector Loc = Pawn->GetActorLocation();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();

		// Require real penetration (a few uu) so pawns standing beside a wall are untouched.
		const double ClosestX = FMath::Clamp(Loc.X, PieceBox.Min.X, PieceBox.Max.X);
		const double ClosestY = FMath::Clamp(Loc.Y, PieceBox.Min.Y, PieceBox.Max.Y);
		const double Dist2D = FVector2D(Loc.X - ClosestX, Loc.Y - ClosestY).Size();
		const bool bXYPenetrating = Dist2D < FMath::Max(Radius - 4.f, 0.f);
		const bool bZPenetrating = (Loc.Z - HalfHeight) < PieceBox.Max.Z - 4.f
		                        && (Loc.Z + HalfHeight) > PieceBox.Min.Z + 4.f;
		if (bXYPenetrating && bZPenetrating)
		{
			// Eject along the SHORTEST escape, not always straight up. The old "teleport to the
			// piece top" flung a pawn overlapping a WALL/ROOF up several metres (Tom: "a bot launches
			// into the air for no reason"). Pick the min-magnitude of {up, ±X, ±Y}: a pawn standing in
			// a floor pops up onto it (up is nearest); a pawn clipping a wall slides out sideways.
			const float PadV = HalfHeight + 2.f;
			const float PadH = Radius + 2.f;
			const float UpEsc = (PieceBox.Max.Z + PadV) - Loc.Z;   // up onto the piece
			const float PushPX = (PieceBox.Max.X + PadH) - Loc.X;
			const float PushNX = Loc.X - (PieceBox.Min.X - PadH);
			const float PushPY = (PieceBox.Max.Y + PadH) - Loc.Y;
			const float PushNY = Loc.Y - (PieceBox.Min.Y - PadH);

			FVector Delta(0.f, 0.f, UpEsc);
			float Best = FMath::Abs(UpEsc);
			auto Consider = [&](const FVector& D)
			{
				if (D.Size() < Best) { Best = D.Size(); Delta = D; }
			};
			Consider(FVector(PushPX, 0.f, 0.f));
			Consider(FVector(-PushNX, 0.f, 0.f));
			Consider(FVector(0.f, PushPY, 0.f));
			Consider(FVector(0.f, -PushNY, 0.f));

			Pawn->SetActorLocation(Loc + Delta,
				/*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
		}
	}
}
