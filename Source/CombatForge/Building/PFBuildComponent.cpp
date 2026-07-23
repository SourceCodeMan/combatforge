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
	// Mouse-wheel cycle: wall family contiguous so scroll-from-Wall hits Window/Door next
	// (players expect "change wall type" without hunting through floor/ramp/roof first).
	constexpr EPFBuildTool GCycleOrder[12] =
	{
		EPFBuildTool::Wall, EPFBuildTool::WallWindow, EPFBuildTool::WallDoor, EPFBuildTool::WallDoorOneWay,
		EPFBuildTool::Floor, EPFBuildTool::FloorTrap, EPFBuildTool::Ramp, EPFBuildTool::Roof,
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
	// Placed-piece fallback (ISM-safe Color tint). Ghost uses a separate translucent parent — BasicShape
	// is fully opaque so alpha on Color never softens the green overlay.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMatFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ArtMatFinder(TEXT("/Game/Materials/M_PF_BuildPiece.M_PF_BuildPiece"));
	// PREFER the project's own cooked translucent (/Game/Materials is force-cooked); the engine DEBUG
	// material is not guaranteed to exist in packaged builds — if it was missing, the ghost silently fell
	// back to the OPAQUE BasicShapeMaterial in every alpha (same failure class as the midline tint screen).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GlassFadeFinder(
		TEXT("/Game/Materials/M_PF_GlassFade.M_PF_GlassFade"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostMatFinder(
		TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
	CubeMesh      = CubeFinder.Object;
	CylinderMesh  = CylinderFinder.Object;
	ConeMesh      = ConeFinder.Object;
	ShapeMaterial = BasicMatFinder.Succeeded() ? BasicMatFinder.Object : ArtMatFinder.Object;
	GhostMaterial = GlassFadeFinder.Succeeded() ? GlassFadeFinder.Object
		: (GhostMatFinder.Succeeded() ? GhostMatFinder.Object : ShapeMaterial);
}

void UPFBuildComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GhostMesh)
	{
		GhostMesh->DestroyComponent();
		GhostMesh = nullptr;
	}
	if (GhostFacingMesh)
	{
		GhostFacingMesh->DestroyComponent();
		GhostFacingMesh = nullptr;
	}
	GhostMID = nullptr;
	GhostFacingMID = nullptr;
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

	// Q tap toggles the build wheel (open / commit). No hold threshold — hold was unreliable.
	EIC->BindAction(Cfg->IA_BuildWheel, ETriggerEvent::Started, this, &UPFBuildComponent::OnWheelTogglePressed);
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
	// Don't steal mouse-wheel while the build wheel is open (cursor aims sectors instead).
	if (bWheelOpenSent)
	{
		return;
	}
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

void UPFBuildComponent::OnWheelTogglePressed()
{
	// Tap Q: open if closed, commit (or cancel in dead zone) if open.
	if (bWheelOpenSent)
	{
		bWheelOpenSent = false;
		OnBuildWheelRequestedEvent.Broadcast(false);
	}
	else
	{
		bWheelOpenSent = true;
		OnBuildWheelRequestedEvent.Broadcast(true);
	}
}

void UPFBuildComponent::NotifyBuildWheelClosed()
{
	// Wheel closed via digit / Esc / phase change — keep our open flag in sync so the next Q opens.
	bWheelOpenSent = false;
}

void UPFBuildComponent::EquipTool(EPFBuildTool Tool)
{
	if (Tool != EquippedTool)
	{
		RampRotOffset = 0;   // ramp offset resets on switch; prop offset persists (§3.5)
	}
	EquippedTool = Tool;
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

	// While the piece wheel is open, freeze placement so LMB / turbo don't fire under the UI.
	if (bWheelOpenSent)
	{
		SetGhostVisible(false);
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
	const FLinearColor Tint = (bDenyFlash || Reason != EPFDenyReason::None)
		? PFColors::GhostInvalid : PFColors::GhostValid;
	ApplyGhostTint(Tint);
	// One-way: front-side wedge so facing is readable through the translucent overlay.
	UpdateOneWayFacingCue(Type, X, Y, Z, Rot, true);
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
	ApplyGhostTint((Now < DenyFlashUntil) ? PFColors::GhostInvalid : PFColors::DeleteHighlight);
	UpdateOneWayFacingCue(Rec.Type, Rec.X, Rec.Y, Rec.Z, Rec.Rot, false);
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

	auto MakeGhostComp = [this](const FName& Name) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetOwner(), Name);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(false);
		Comp->SetCanEverAffectNavigation(false);
		Comp->SetVisibility(false);
		// Translucent sort: draw after world so the soft green reads as an overlay.
		Comp->SetTranslucentSortPriority(100);
		Comp->SetRenderCustomDepth(false);
		Comp->RegisterComponent();
		return Comp;
	};

	GhostMesh = MakeGhostComp(TEXT("BuildGhost"));
	GhostFacingMesh = MakeGhostComp(TEXT("BuildGhostFacing"));
	if (ConeMesh)
	{
		GhostFacingMesh->SetStaticMesh(ConeMesh);
	}
	else if (CubeMesh)
	{
		GhostFacingMesh->SetStaticMesh(CubeMesh);
	}

	UMaterialInterface* Parent = GhostMaterial ? GhostMaterial.Get() : ShapeMaterial.Get();
	if (Parent)
	{
		GhostMID = UMaterialInstanceDynamic::Create(Parent, this);
		GhostFacingMID = UMaterialInstanceDynamic::Create(Parent, this);
		ApplyGhostTint(PFColors::GhostValid);
		if (GhostMesh)
		{
			GhostMesh->SetMaterial(0, GhostMID);
		}
		if (GhostFacingMesh)
		{
			GhostFacingMesh->SetMaterial(0, GhostFacingMID);
		}
	}
}

void UPFBuildComponent::ApplyGhostTint(const FLinearColor& Color)
{
	auto ApplyTo = [&](UMaterialInstanceDynamic* MID)
	{
		if (!MID)
		{
			return;
		}
		// M_SimpleUnlitTranslucent uses Color (RGB + A). Also set common opacity aliases.
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		MID->SetVectorParameterValue(TEXT("BaseColor"), Color);
		MID->SetScalarParameterValue(TEXT("Opacity"), Color.A);
		MID->SetScalarParameterValue(TEXT("OpacityMultiplier"), Color.A);
	};
	ApplyTo(GhostMID);
	ApplyTo(GhostFacingMID);
}

void UPFBuildComponent::UpdateOneWayFacingCue(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot, bool bShow)
{
	if (!GhostFacingMesh)
	{
		return;
	}
	const bool bOneWay = bShow && Type == EPFPieceType::WallDoorOneWay;
	if (!bOneWay)
	{
		GhostFacingMesh->SetVisibility(false);
		return;
	}

	// Door leaf center on the wall plane, then push a small cone outward on the FRONT (+normal).
	// Front = +Y for N-edge (Rot 0), +X for E-edge (Rot 1) — matches APFBuildPieceActor::WallFrontNormal.
	const float S = 100.f;
	const float Wx = X * S;
	const float Wy = Y * S;
	const float Wz = Z * S;
	const float Cell = 400.f;
	const float DoorH = 230.f;
	FVector Center;
	FVector Front;
	FRotator ConeRot;
	if (Rot == 1)
	{
		Center = FVector(Wx + Cell, Wy + Cell * 0.5f, Wz + DoorH * 0.5f);
		Front = FVector(1.f, 0.f, 0.f);
		// Engine cone points +Z; pitch -90 aims +X (front).
		ConeRot = FRotator(-90.f, 0.f, 0.f);
	}
	else
	{
		Center = FVector(Wx + Cell * 0.5f, Wy + Cell, Wz + DoorH * 0.5f);
		Front = FVector(0.f, 1.f, 0.f);
		// Pitch -90 then yaw 90 → aim +Y.
		ConeRot = FRotator(-90.f, 90.f, 0.f);
	}
	// Sit just in front of the door face so it reads as "open this side".
	const FVector Tip = Center + Front * 55.f;
	GhostFacingMesh->SetWorldLocation(Tip);
	GhostFacingMesh->SetWorldRotation(ConeRot);
	GhostFacingMesh->SetWorldScale3D(FVector(0.35f, 0.35f, 0.55f));
	GhostFacingMesh->SetVisibility(true);
}

void UPFBuildComponent::SetGhostVisible(bool bVisible)
{
	if (GhostMesh && GhostMesh->IsVisible() != bVisible)
	{
		GhostMesh->SetVisibility(bVisible);
	}
	if (!bVisible && GhostFacingMesh)
	{
		GhostFacingMesh->SetVisibility(false);
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
		// Soft translucent color only — do NOT rebind warehouse surfaces (those are opaque and
		// bury the validity tint / hide one-way facing under solid concrete).
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
	// Shared catalog: warehouse barrel/crate/boxes for props; cube for structural (incl. flat roof).
	if (UStaticMesh* Shared = PFBuildPieceVisuals::MeshForType(Type))
	{
		return Shared;
	}
	switch (Type)
	{
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
