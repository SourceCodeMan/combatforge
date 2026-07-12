// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildPieceVisuals.h"

#include "PaintForge.h"
#include "Building/PFGridMath.h"

#include "Engine/StaticMesh.h"
#include "UObject/SoftObjectPath.h"

namespace PFBuildPieceVisuals
{
namespace
{
	struct FPropSlot
	{
		/** Soft paths tried in order (first success wins). */
		const TCHAR* Paths[3] = { nullptr, nullptr, nullptr };
		/** Design footprint (full size uu) matching FPFGridMath prop AABBs. */
		FVector TargetSize = FVector(120.f, 120.f, 220.f);
		UStaticMesh* Mesh = nullptr;
		FVector FitScale = FVector::OneVector;
		FVector BoundsOrigin = FVector::ZeroVector;
		FVector BoundsExtent = FVector(50.f, 50.f, 50.f);
		bool bWarehouse = false;
	};

	bool GLoaded = false;
	FPropSlot GBarrel;   // PropCan
	FPropSlot GCrate;    // PropDorito
	FPropSlot GBoxes;    // PropSnake
	UStaticMesh* GCube = nullptr;
	UStaticMesh* GCylinder = nullptr;
	UStaticMesh* GCone = nullptr;

	UStaticMesh* SoftLoadMesh(const TCHAR* Path)
	{
		if (!Path || !*Path)
		{
			return nullptr;
		}
		const FSoftObjectPath Soft(Path);
		return Cast<UStaticMesh>(Soft.TryLoad());
	}

	void FitSlot(FPropSlot& Slot, UStaticMesh* Fallback)
	{
		UStaticMesh* Chosen = nullptr;
		for (const TCHAR* Path : Slot.Paths)
		{
			if (UStaticMesh* M = SoftLoadMesh(Path))
			{
				Chosen = M;
				Slot.bWarehouse = true;
				break;
			}
		}
		if (!Chosen)
		{
			Chosen = Fallback;
			Slot.bWarehouse = false;
		}
		Slot.Mesh = Chosen;
		if (!Chosen)
		{
			return;
		}

		const FBoxSphereBounds B = Chosen->GetBounds();
		Slot.BoundsOrigin = B.Origin;
		Slot.BoundsExtent = B.BoxExtent;
		const FVector MeshSize(
			FMath::Max(B.BoxExtent.X * 2.f, 1.f),
			FMath::Max(B.BoxExtent.Y * 2.f, 1.f),
			FMath::Max(B.BoxExtent.Z * 2.f, 1.f));

		if (Slot.bWarehouse)
		{
			// Fit mesh into the gameplay footprint used by placement AABBs.
			Slot.FitScale = FVector(
				Slot.TargetSize.X / MeshSize.X,
				Slot.TargetSize.Y / MeshSize.Y,
				Slot.TargetSize.Z / MeshSize.Z);
			// Keep proportions sensible — clamp axis ratio so barrels don't pancake.
			const float MaxAxis = FMath::Max3(Slot.FitScale.X, Slot.FitScale.Y, Slot.FitScale.Z);
			const float MinAxis = FMath::Min3(Slot.FitScale.X, Slot.FitScale.Y, Slot.FitScale.Z);
			if (MaxAxis > MinAxis * 2.5f)
			{
				// Prefer height fit for upright props; longest horizontal for stacks.
				if (Slot.TargetSize.Z >= Slot.TargetSize.X)
				{
					const float Sc = Slot.TargetSize.Z / MeshSize.Z;
					Slot.FitScale = FVector(Sc, Sc, Sc);
				}
				else
				{
					const float Sc = FMath::Min(Slot.TargetSize.X / MeshSize.X, Slot.TargetSize.Y / MeshSize.Y);
					Slot.FitScale = FVector(Sc, Sc, Sc);
				}
			}
		}
		else
		{
			// Engine basic-shape fallbacks use the legacy baked scales in PieceLocalTransform.
			Slot.FitScale = FVector::OneVector;
		}
	}

	const FPropSlot* SlotForProp(EPFPieceType Type)
	{
		switch (Type)
		{
		case EPFPieceType::PropCan:    return &GBarrel;
		case EPFPieceType::PropDorito: return &GCrate;
		case EPFPieceType::PropSnake:  return &GBoxes;
		default:                       return nullptr;
		}
	}

	FTransform BasicShapeTransform(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot)
	{
		return FPFGridMath::PieceLocalTransform(Type, X, Y, Z, Rot);
	}

	FTransform WarehousePropTransform(const FPropSlot& Slot, int16 X, int16 Y, int16 Z, uint8 Rot)
	{
		const float S = static_cast<float>(PFGrid::SubUU);
		const float Wx = X * S;
		const float Wy = Y * S;
		const float Wz = Z * S;

		// Pivot so the mesh bottom sits on the support plane (Wz).
		const FVector& O = Slot.BoundsOrigin;
		const FVector& E = Slot.BoundsExtent;
		const FVector& Sc = Slot.FitScale;
		const FVector Loc(
			Wx - O.X * Sc.X,
			Wy - O.Y * Sc.Y,
			Wz - (O.Z - E.Z) * Sc.Z);

		return FTransform(FRotator(0.f, Rot * 90.f, 0.f), Loc, Sc);
	}
} // namespace

const TCHAR* DisplayName(EPFPieceType Type)
{
	switch (Type)
	{
	case EPFPieceType::Wall:       return TEXT("Wall");
	case EPFPieceType::Floor:      return TEXT("Floor");
	case EPFPieceType::Ramp:       return TEXT("Ramp");
	case EPFPieceType::Roof:       return TEXT("Roof");
	case EPFPieceType::PropCan:    return TEXT("Barrel");
	case EPFPieceType::PropDorito: return TEXT("Crate");
	case EPFPieceType::PropSnake:  return TEXT("Boxes");
	default:                       return TEXT("?");
	}
}

const TCHAR* DisplayName(EPFBuildTool Tool)
{
	switch (Tool)
	{
	case EPFBuildTool::Wall:       return TEXT("Wall");
	case EPFBuildTool::Floor:      return TEXT("Floor");
	case EPFBuildTool::Ramp:       return TEXT("Ramp");
	case EPFBuildTool::Roof:       return TEXT("Roof");
	case EPFBuildTool::PropCan:    return TEXT("Barrel");
	case EPFBuildTool::PropDorito: return TEXT("Crate");
	case EPFBuildTool::PropSnake:  return TEXT("Boxes");
	case EPFBuildTool::Delete:     return TEXT("Delete");
	default:                       return TEXT("?");
	}
}

void EnsureLoaded()
{
	if (GLoaded)
	{
		return;
	}
	GLoaded = true;

	// Engine fallbacks (always present). Soft-load so EnsureLoaded is safe outside constructors.
	GCube = SoftLoadMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	GCylinder = SoftLoadMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	GCone = SoftLoadMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));

	// PropCan → metal / plastic barrel (upright cover).
	GBarrel.TargetSize = FVector(120.f, 120.f, 220.f);
	GBarrel.Paths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Aba_Storage_Barrel_Metal_Blue_01/SM_Ind_Aba_Storage_Barrel_Metal_Blue_01.SM_Ind_Aba_Storage_Barrel_Metal_Blue_01");
	GBarrel.Paths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Barrel_Plastic_Blue_01/SM_Ind_War_Storage_Barrel_Plastic_Blue_01.SM_Ind_War_Storage_Barrel_Plastic_Blue_01");
	GBarrel.Paths[2] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Sto_Barrel_Metal_Rust_03/SM_Ind_Sto_Barrel_Metal_Rust_03.SM_Ind_Sto_Barrel_Metal_Rust_03");
	FitSlot(GBarrel, GCylinder);

	// PropDorito → plastic crate (mid cover, was "cone").
	GCrate.TargetSize = FVector(240.f, 240.f, 200.f);
	GCrate.Paths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Crate_Plastic_Blue_01/SM_Ind_War_Storage_Crate_Plastic_Blue_01.SM_Ind_War_Storage_Crate_Plastic_Blue_01");
	GCrate.Paths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Crate_Plastic_Teal_01/SM_Ind_War_Storage_Crate_Plastic_Teal_01.SM_Ind_War_Storage_Crate_Plastic_Teal_01");
	GCrate.Paths[2] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Crate_Trap_Covered_01/SM_Ind_War_Storage_Crate_Trap_Covered_01.SM_Ind_War_Storage_Crate_Trap_Covered_01");
	FitSlot(GCrate, GCone);

	// PropSnake → cardboard box stack (long low cover).
	GBoxes.TargetSize = FVector(400.f, 120.f, 120.f);
	GBoxes.Paths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Set_01/SM_Ind_War_Storage_Box_Cardboard_Set_01_A.SM_Ind_War_Storage_Box_Cardboard_Set_01_A");
	GBoxes.Paths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Set_02/SM_Ind_War_Storage_Box_Cardboard_Set_02_A.SM_Ind_War_Storage_Box_Cardboard_Set_02_A");
	GBoxes.Paths[2] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Box_Cardboard_Worn_02/SM_Ind_War_Storage_Box_Cardboard_Worn_02.SM_Ind_War_Storage_Box_Cardboard_Worn_02");
	FitSlot(GBoxes, GCube);

	UE_LOG(PaintForgeLog, Log,
		TEXT("BuildPieceVisuals: Barrel=%s Crate=%s Boxes=%s (warehouse=%d/%d/%d)"),
		GBarrel.Mesh ? *GBarrel.Mesh->GetName() : TEXT("null"),
		GCrate.Mesh ? *GCrate.Mesh->GetName() : TEXT("null"),
		GBoxes.Mesh ? *GBoxes.Mesh->GetName() : TEXT("null"),
		GBarrel.bWarehouse ? 1 : 0, GCrate.bWarehouse ? 1 : 0, GBoxes.bWarehouse ? 1 : 0);
}

UStaticMesh* MeshForType(EPFPieceType Type)
{
	EnsureLoaded();
	if (const FPropSlot* Slot = SlotForProp(Type))
	{
		return Slot->Mesh;
	}
	switch (Type)
	{
	case EPFPieceType::Roof:
		return GCone ? GCone : GCube;
	default:
		return GCube;
	}
}

bool UsesNativeMaterials(EPFPieceType Type)
{
	EnsureLoaded();
	if (const FPropSlot* Slot = SlotForProp(Type))
	{
		return Slot->bWarehouse && Slot->Mesh != nullptr;
	}
	return false;
}

FTransform PieceWorldTransform(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot)
{
	EnsureLoaded();
	if (const FPropSlot* Slot = SlotForProp(Type))
	{
		if (Slot->bWarehouse && Slot->Mesh)
		{
			return WarehousePropTransform(*Slot, X, Y, Z, Rot);
		}
	}
	return BasicShapeTransform(Type, X, Y, Z, Rot);
}
} // namespace PFBuildPieceVisuals
