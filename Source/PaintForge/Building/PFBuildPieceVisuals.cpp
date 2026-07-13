// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildPieceVisuals.h"

#include "PaintForge.h"
#include "Building/PFGridMath.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceDynamic.h"
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

	/** Per structural type: soft texture set + MID knobs for M_PF_BuildPiece. */
	struct FSurfaceProfile
	{
		const TCHAR* Label = TEXT("concrete");
		/** Diffuse / base color candidates (warehouse first, ambientCG fallback). */
		const TCHAR* BaseColorPaths[3] = { nullptr, nullptr, nullptr };
		const TCHAR* NormalPaths[3] = { nullptr, nullptr, nullptr };
		/** ORD or grayscale rough (G=rough or R=rough depending on pack). */
		const TCHAR* OrdPaths[3] = { nullptr, nullptr, nullptr };
		float WorldTileSize = 256.f;
		float AccentBoost = 2.0f;
		UTexture* BaseColor = nullptr;
		UTexture* Normal = nullptr;
		UTexture* ORD = nullptr;
		bool bLoaded = false;
	};

	bool GLoaded = false;
	FPropSlot GBarrel;   // PropCan
	FPropSlot GCrate;    // PropDorito
	FPropSlot GBoxes;    // PropSnake
	UStaticMesh* GCube = nullptr;
	UStaticMesh* GCylinder = nullptr;
	UStaticMesh* GCone = nullptr;

	FSurfaceProfile GSurfWall;
	FSurfaceProfile GSurfFloor;
	FSurfaceProfile GSurfRamp;
	FSurfaceProfile GSurfRoof;
	FSurfaceProfile GSurfFallback; // Concrete034 / flat

	UStaticMesh* SoftLoadMesh(const TCHAR* Path)
	{
		if (!Path || !*Path)
		{
			return nullptr;
		}
		const FSoftObjectPath Soft(Path);
		return Cast<UStaticMesh>(Soft.TryLoad());
	}

	UTexture* SoftLoadTexture(const TCHAR* Path)
	{
		if (!Path || !*Path)
		{
			return nullptr;
		}
		const FSoftObjectPath Soft(Path);
		return Cast<UTexture>(Soft.TryLoad());
	}

	UTexture* SoftLoadFirstTexture(const TCHAR* const Paths[3])
	{
		for (int32 i = 0; i < 3; ++i)
		{
			if (UTexture* T = SoftLoadTexture(Paths[i]))
			{
				return T;
			}
		}
		return nullptr;
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

	void LoadSurface(FSurfaceProfile& S)
	{
		S.BaseColor = SoftLoadFirstTexture(S.BaseColorPaths);
		S.Normal = SoftLoadFirstTexture(S.NormalPaths);
		S.ORD = SoftLoadFirstTexture(S.OrdPaths);
		S.bLoaded = (S.BaseColor != nullptr);
	}

	void InitSurfaceProfiles()
	{
		// AmbientCG Concrete034 (always preferred as last path — ships with project).
		static const TCHAR* CC0_BC = TEXT("/Game/Textures/Concrete/T_Concrete034_Color.T_Concrete034_Color");
		static const TCHAR* CC0_N  = TEXT("/Game/Textures/Concrete/T_Concrete034_Normal.T_Concrete034_Normal");
		static const TCHAR* CC0_R  = TEXT("/Game/Textures/Concrete/T_Concrete034_Rough.T_Concrete034_Rough");

		// Wall — facade concrete (warehouse) → CC0
		GSurfWall.Label = TEXT("wall-concrete");
		GSurfWall.WorldTileSize = 200.f;
		GSurfWall.AccentBoost = 2.4f;
		GSurfWall.BaseColorPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/T_Ind_War_Wall_Facade_Concrete_New_01_D.T_Ind_War_Wall_Facade_Concrete_New_01_D");
		GSurfWall.BaseColorPaths[1] = CC0_BC;
		GSurfWall.NormalPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/T_Ind_War_Wall_Facade_Concrete_New_01_N.T_Ind_War_Wall_Facade_Concrete_New_01_N");
		GSurfWall.NormalPaths[1] = CC0_N;
		GSurfWall.OrdPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/T_Ind_War_Wall_Facade_Concrete_New_01_ORDp.T_Ind_War_Wall_Facade_Concrete_New_01_ORDp");
		GSurfWall.OrdPaths[1] = CC0_R;
		LoadSurface(GSurfWall);

		// Floor — smooth warehouse concrete, larger tiles, softer team glow
		GSurfFloor.Label = TEXT("floor-concrete");
		GSurfFloor.WorldTileSize = 360.f;
		GSurfFloor.AccentBoost = 1.35f;
		GSurfFloor.BaseColorPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_D.T_Ind_War_Floor_Concrete_Smooth_01_D");
		GSurfFloor.BaseColorPaths[1] = CC0_BC;
		GSurfFloor.NormalPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_N.T_Ind_War_Floor_Concrete_Smooth_01_N");
		GSurfFloor.NormalPaths[1] = CC0_N;
		GSurfFloor.OrdPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_ORDp.T_Ind_War_Floor_Concrete_Smooth_01_ORDp");
		GSurfFloor.OrdPaths[1] = CC0_R;
		LoadSurface(GSurfFloor);

		// Ramp — rusty sheet metal (airsoft bunker plank)
		GSurfRamp.Label = TEXT("ramp-metal");
		GSurfRamp.WorldTileSize = 180.f;
		GSurfRamp.AccentBoost = 2.2f;
		GSurfRamp.BaseColorPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/T_Ind_War_Sheet_Metal_Rusty_01_D.T_Ind_War_Sheet_Metal_Rusty_01_D");
		GSurfRamp.BaseColorPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Garage_Door_Metal_Worn_01/T_Ind_War_Garage_Door_Metal_Worn_01_D.T_Ind_War_Garage_Door_Metal_Worn_01_D");
		GSurfRamp.BaseColorPaths[2] = CC0_BC;
		GSurfRamp.NormalPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/T_Ind_War_Sheet_Metal_Rusty_01_N.T_Ind_War_Sheet_Metal_Rusty_01_N");
		GSurfRamp.NormalPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Garage_Door_Metal_Worn_01/T_Ind_War_Garage_Door_Metal_Worn_01_N.T_Ind_War_Garage_Door_Metal_Worn_01_N");
		GSurfRamp.NormalPaths[2] = CC0_N;
		GSurfRamp.OrdPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/T_Ind_War_Sheet_Metal_Rusty_01_ORDp.T_Ind_War_Sheet_Metal_Rusty_01_ORDp");
		GSurfRamp.OrdPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Garage_Door_Metal_Worn_01/T_Ind_War_Garage_Door_Metal_Worn_01_ORDp.T_Ind_War_Garage_Door_Metal_Worn_01_ORDp");
		GSurfRamp.OrdPaths[2] = CC0_R;
		LoadSurface(GSurfRamp);

		// Roof — painted metal / corrugated roofing
		GSurfRoof.Label = TEXT("roof-metal");
		GSurfRoof.WorldTileSize = 220.f;
		GSurfRoof.AccentBoost = 2.0f;
		GSurfRoof.BaseColorPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/T_Ind_War_Roof_Painted_01_D.T_Ind_War_Roof_Painted_01_D");
		GSurfRoof.BaseColorPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/T_Ind_War_Sheet_Metal_Rusty_01_D.T_Ind_War_Sheet_Metal_Rusty_01_D");
		GSurfRoof.BaseColorPaths[2] = CC0_BC;
		GSurfRoof.NormalPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/T_Ind_War_Roof_Painted_01_N.T_Ind_War_Roof_Painted_01_N");
		GSurfRoof.NormalPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/T_Ind_War_Sheet_Metal_Rusty_01_N.T_Ind_War_Sheet_Metal_Rusty_01_N");
		GSurfRoof.NormalPaths[2] = CC0_N;
		GSurfRoof.OrdPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/T_Ind_War_Roof_Painted_01_ORDp.T_Ind_War_Roof_Painted_01_ORDp");
		GSurfRoof.OrdPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/T_Ind_War_Sheet_Metal_Rusty_01_ORDp.T_Ind_War_Sheet_Metal_Rusty_01_ORDp");
		GSurfRoof.OrdPaths[2] = CC0_R;
		LoadSurface(GSurfRoof);

		// Shared fallback for unknown types / ghost default
		GSurfFallback.Label = TEXT("fallback-concrete");
		GSurfFallback.WorldTileSize = 256.f;
		GSurfFallback.AccentBoost = 1.8f;
		GSurfFallback.BaseColorPaths[0] = CC0_BC;
		GSurfFallback.NormalPaths[0] = CC0_N;
		GSurfFallback.OrdPaths[0] = CC0_R;
		LoadSurface(GSurfFallback);
	}

	const FSurfaceProfile& SurfaceForType(EPFPieceType Type)
	{
		switch (Type)
		{
		case EPFPieceType::Wall:  return GSurfWall;
		case EPFPieceType::Floor: return GSurfFloor;
		case EPFPieceType::Ramp:  return GSurfRamp;
		case EPFPieceType::Roof:  return GSurfRoof;
		default:                  return GSurfFallback;
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

	// ---- Structural surface profiles (materials; geometry stays basic shapes) ----
	InitSurfaceProfiles();

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

	// These globals are plain file-static structs — invisible to the garbage collector. Without rooting, a GC
	// pass mid-session frees the loaded textures/meshes and the raw pointers dangle, so the next ghost update
	// hands a freed UTexture to SetTextureParameterValue and crashes in CoreUObject. Root them for the session.
	auto Keep = [](UObject* Obj)
	{
		if (Obj != nullptr && !Obj->IsRooted())
		{
			Obj->AddToRoot();
		}
	};
	Keep(GCube); Keep(GCylinder); Keep(GCone);
	Keep(GBarrel.Mesh); Keep(GCrate.Mesh); Keep(GBoxes.Mesh);
	for (FSurfaceProfile* P : { &GSurfWall, &GSurfFloor, &GSurfRamp, &GSurfRoof, &GSurfFallback })
	{
		Keep(P->BaseColor); Keep(P->Normal); Keep(P->ORD);
	}

	UE_LOG(PaintForgeLog, Log,
		TEXT("BuildPieceVisuals: props Barrel=%s Crate=%s Boxes=%s (wh=%d/%d/%d) | surfaces Wall=%s Floor=%s Ramp=%s Roof=%s (tex=%d/%d/%d/%d)"),
		GBarrel.Mesh ? *GBarrel.Mesh->GetName() : TEXT("null"),
		GCrate.Mesh ? *GCrate.Mesh->GetName() : TEXT("null"),
		GBoxes.Mesh ? *GBoxes.Mesh->GetName() : TEXT("null"),
		GBarrel.bWarehouse ? 1 : 0, GCrate.bWarehouse ? 1 : 0, GBoxes.bWarehouse ? 1 : 0,
		GSurfWall.Label, GSurfFloor.Label, GSurfRamp.Label, GSurfRoof.Label,
		GSurfWall.BaseColor ? 1 : 0, GSurfFloor.BaseColor ? 1 : 0,
		GSurfRamp.BaseColor ? 1 : 0, GSurfRoof.BaseColor ? 1 : 0);
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
	// Structural always uses M_PF_BuildPiece MIDs (team Color + surface profiles).
	return false;
}

void ApplyStructuralSurface(UMaterialInstanceDynamic* MID, EPFPieceType Type)
{
	if (!MID)
	{
		return;
	}
	EnsureLoaded();

	// Props keep native mats when warehouse-loaded; only structural (+ ghost) use this.
	if (PFIsProp(Type))
	{
		return;
	}

	const FSurfaceProfile& S = SurfaceForType(Type);

	// TextureObjectParameter names from Scripts/create_build_material.py.
	// Soft-fail if the master material is the BasicShape fallback (no params) — Color still works.
	if (S.BaseColor)
	{
		MID->SetTextureParameterValue(TEXT("BaseColorTex"), S.BaseColor);
	}
	if (S.Normal)
	{
		MID->SetTextureParameterValue(TEXT("NormalTex"), S.Normal);
	}
	if (S.ORD)
	{
		// Master material samples ORDTex (G=rough); also set RoughTex if arena-style masters ever share the MID.
		MID->SetTextureParameterValue(TEXT("ORDTex"), S.ORD);
		MID->SetTextureParameterValue(TEXT("RoughTex"), S.ORD);
	}
	MID->SetScalarParameterValue(TEXT("WorldTileSize"), S.WorldTileSize);
	MID->SetScalarParameterValue(TEXT("AccentBoost"), S.AccentBoost);
}

const TCHAR* StructuralSurfaceName(EPFPieceType Type)
{
	EnsureLoaded();
	return SurfaceForType(Type).Label;
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
