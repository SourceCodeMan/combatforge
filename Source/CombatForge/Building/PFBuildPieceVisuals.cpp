// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildPieceVisuals.h"

#include "CombatForge.h"
#include "Building/PFGridMath.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/BodySetup.h"
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

	/** Per palette role: warehouse textures on triplanar masters (never M_PF_BuildPiece). */
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
		/** Optional albedo multiply for cohesion (e.g. lift dark facade maps). */
		FLinearColor AlbedoTint = FLinearColor::White;
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

	// Triplanar masters (world-aligned — correct on non-uniform scaled cubes).
	UMaterialInterface* GMasterFloor = nullptr;  // M_PF_ArenaFloor
	UMaterialInterface* GMasterWall = nullptr;   // M_PF_ArenaWall
	// Metal master is procedural (no tex params) — rebind metal maps onto floor triplanar instead.
	UMaterialInterface* GMasterMetalTri = nullptr;

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

	// Megascans warehouse props import with NO simple collision (or collision authored off), so the build
	// ISM's QueryAndPhysics + Block(Pawn/Paintball) responses hit nothing → pawns AND projectiles pass
	// straight through the barrels/crates/boxes. Give the mesh one box primitive matching its bounds so
	// every instanced copy blocks. A box is analytic (no cook) and, unlike the mesh's own missing complex
	// collision, always answers pawn capsule sweeps + paintball traces. Simple-as-complex so line traces
	// (LOS / hitscan-style checks) resolve to the box too. Idempotent: skips meshes that already collide
	// (the engine BasicShape fallbacks), and skips if the box was already added this session.
	void EnsureSimpleBoxCollision(UStaticMesh* Mesh)
	{
		if (Mesh == nullptr)
		{
			return;
		}
		UBodySetup* BS = Mesh->GetBodySetup();
		if (BS == nullptr)
		{
			Mesh->CreateBodySetup();
			BS = Mesh->GetBodySetup();
		}
		if (BS == nullptr || BS->AggGeom.GetElementCount() > 0)
		{
			return;   // no body setup available, or it already has simple collision → leave it be
		}
		const FBoxSphereBounds B = Mesh->GetBounds();
		FKBoxElem Box(B.BoxExtent.X * 2.f, B.BoxExtent.Y * 2.f, B.BoxExtent.Z * 2.f);
		Box.Center = B.Origin;
		BS->AggGeom.BoxElems.Add(Box);
		BS->CollisionTraceFlag = CTF_UseSimpleAsComplex;   // the box also answers complex (line-trace) queries
		BS->InvalidatePhysicsData();   // frees any already-cooked body, resets bCreatedPhysicsMeshes
		BS->CreatePhysicsMeshes();     // rebuild the runtime body now including the new box
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

		// Warehouse props need collision injected (see EnsureSimpleBoxCollision); engine fallbacks already have it.
		if (Slot.bWarehouse)
		{
			EnsureSimpleBoxCollision(Chosen);
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
		// AmbientCG Concrete034 (ships with project — last-resort when warehouse pack missing).
		static const TCHAR* CC0_BC = TEXT("/Game/Textures/Concrete/T_Concrete034_Color.T_Concrete034_Color");
		static const TCHAR* CC0_N  = TEXT("/Game/Textures/Concrete/T_Concrete034_Normal.T_Concrete034_Normal");
		static const TCHAR* CC0_R  = TEXT("/Game/Textures/Concrete/T_Concrete034_Rough.T_Concrete034_Rough");

		// Wall — facade concrete; slight albedo lift (raw Megascans wall MI read near-black on cubes).
		GSurfWall.Label = TEXT("wall-concrete");
		GSurfWall.WorldTileSize = 220.f;
		GSurfWall.AccentBoost = 2.2f;
		GSurfWall.AlbedoTint = FLinearColor(1.08f, 1.06f, 1.04f);   // mild lift only (1.35 washed walls)
		GSurfWall.BaseColorPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/T_Ind_War_Wall_Facade_Concrete_New_01_D.T_Ind_War_Wall_Facade_Concrete_New_01_D");
		// Floor smooth as secondary — same warehouse family if facade missing.
		GSurfWall.BaseColorPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_D.T_Ind_War_Floor_Concrete_Smooth_01_D");
		GSurfWall.BaseColorPaths[2] = CC0_BC;
		GSurfWall.NormalPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/T_Ind_War_Wall_Facade_Concrete_New_01_N.T_Ind_War_Wall_Facade_Concrete_New_01_N");
		GSurfWall.NormalPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_N.T_Ind_War_Floor_Concrete_Smooth_01_N");
		GSurfWall.NormalPaths[2] = CC0_N;
		GSurfWall.OrdPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Wall_Facade_Concrete_New_01/T_Ind_War_Wall_Facade_Concrete_New_01_ORDp.T_Ind_War_Wall_Facade_Concrete_New_01_ORDp");
		GSurfWall.OrdPaths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_ORDp.T_Ind_War_Floor_Concrete_Smooth_01_ORDp");
		GSurfWall.OrdPaths[2] = CC0_R;
		LoadSurface(GSurfWall);

		// Floor — smooth warehouse concrete, larger tiles
		GSurfFloor.Label = TEXT("floor-concrete");
		GSurfFloor.WorldTileSize = 380.f;
		GSurfFloor.AccentBoost = 1.25f;
		GSurfFloor.AlbedoTint = FLinearColor(0.95f, 0.95f, 0.94f);
		GSurfFloor.BaseColorPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_D.T_Ind_War_Floor_Concrete_Smooth_01_D");
		GSurfFloor.BaseColorPaths[1] = CC0_BC;
		GSurfFloor.NormalPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_N.T_Ind_War_Floor_Concrete_Smooth_01_N");
		GSurfFloor.NormalPaths[1] = CC0_N;
		GSurfFloor.OrdPaths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/T_Ind_War_Floor_Concrete_Smooth_01_ORDp.T_Ind_War_Floor_Concrete_Smooth_01_ORDp");
		GSurfFloor.OrdPaths[1] = CC0_R;
		LoadSurface(GSurfFloor);

		// Ramp — rusty sheet metal (airsoft bunker plank)
		GSurfRamp.Label = TEXT("ramp-metal");
		GSurfRamp.WorldTileSize = 160.f;
		GSurfRamp.AccentBoost = 2.0f;
		GSurfRamp.AlbedoTint = FLinearColor(1.05f, 1.0f, 0.95f);
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

		// Roof — painted metal (distinct from rusty ramp)
		GSurfRoof.Label = TEXT("roof-metal");
		GSurfRoof.WorldTileSize = 200.f;
		GSurfRoof.AccentBoost = 1.8f;
		GSurfRoof.AlbedoTint = FLinearColor(0.9f, 0.92f, 0.95f);
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

	const FSurfaceProfile& SurfaceForRole(EPFSurfaceRole Role)
	{
		switch (Role)
		{
		case EPFSurfaceRole::WallConcrete:  return GSurfWall;
		case EPFSurfaceRole::FloorConcrete: return GSurfFloor;
		case EPFSurfaceRole::MetalRusty:    return GSurfRamp;
		case EPFSurfaceRole::MetalRoof:     return GSurfRoof;
		default:                            return GSurfFallback;
		}
	}

	const FSurfaceProfile& SurfaceForType(EPFPieceType Type)
	{
		return SurfaceForRole(RoleForPieceType(Type));
	}

	void ApplyProfileToMID(UMaterialInstanceDynamic* MID, const FSurfaceProfile& S)
	{
		if (!MID)
		{
			return;
		}
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
			MID->SetTextureParameterValue(TEXT("ORDTex"), S.ORD);
			MID->SetTextureParameterValue(TEXT("RoughTex"), S.ORD);
		}
		MID->SetScalarParameterValue(TEXT("WorldTileSize"), S.WorldTileSize);
		MID->SetScalarParameterValue(TEXT("AccentBoost"), S.AccentBoost);
		// Arena masters bake a constant albedo multiply; Color is only used for emissive on marks/metal.
		// Some masters also accept a soft "Tint" — try both safely.
		MID->SetVectorParameterValue(TEXT("Tint"), S.AlbedoTint);
	}

	UMaterialInterface* MasterForRole(EPFSurfaceRole Role)
	{
		switch (Role)
		{
		case EPFSurfaceRole::WallConcrete:
			return GMasterWall ? GMasterWall : GMasterFloor;
		case EPFSurfaceRole::FloorConcrete:
			// WORKAROUND: M_PF_ArenaFloor renders BLACK — its base-color graph is mis-wired (separate from
			// the roughness Clamp), unlike M_PF_ArenaWall which is correct. Drive the floor through the
			// working WALL master + the floor-concrete textures (world-aligned projection is orientation-
			// agnostic, so it lands right on a horizontal surface). Revert to GMasterFloor once the floor
			// material's base-color path is repaired in-editor.
			return GMasterWall ? GMasterWall : GMasterFloor;
		case EPFSurfaceRole::MetalRusty:
		case EPFSurfaceRole::MetalRoof:
			// Rebind metal maps onto floor triplanar (has BaseColorTex/NormalTex/RoughTex params).
			return GMasterMetalTri ? GMasterMetalTri : (GMasterFloor ? GMasterFloor : GMasterWall);
		default:
			return GMasterFloor ? GMasterFloor : GMasterWall;
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

	// Triplanar masters — world-aligned so scaled arena/build cubes don't smear like mesh-UV Megascans MIs.
	// Intentionally NOT M_PF_BuildPiece (ISM checker + local-edit guardrail).
	GMasterFloor = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaFloor.M_PF_ArenaFloor")).TryLoad());
	GMasterWall = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaWall.M_PF_ArenaWall")).TryLoad());
	// Floor master doubles as metal carrier (has texture params; ArenaMetal is procedural-only).
	GMasterMetalTri = GMasterFloor;

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
	Keep(GMasterFloor); Keep(GMasterWall);
	for (FSurfaceProfile* P : { &GSurfWall, &GSurfFloor, &GSurfRamp, &GSurfRoof, &GSurfFallback })
	{
		Keep(P->BaseColor); Keep(P->Normal); Keep(P->ORD);
	}

	UE_LOG(CombatForgeLog, Log,
		TEXT("BuildPieceVisuals: props Barrel=%s Crate=%s Boxes=%s (wh=%d/%d/%d) | palette Wall=%s Floor=%s Ramp=%s Roof=%s (tex=%d/%d/%d/%d) masters floor=%d wall=%d"),
		GBarrel.Mesh ? *GBarrel.Mesh->GetName() : TEXT("null"),
		GCrate.Mesh ? *GCrate.Mesh->GetName() : TEXT("null"),
		GBoxes.Mesh ? *GBoxes.Mesh->GetName() : TEXT("null"),
		GBarrel.bWarehouse ? 1 : 0, GCrate.bWarehouse ? 1 : 0, GBoxes.bWarehouse ? 1 : 0,
		GSurfWall.Label, GSurfFloor.Label, GSurfRamp.Label, GSurfRoof.Label,
		GSurfWall.BaseColor ? 1 : 0, GSurfFloor.BaseColor ? 1 : 0,
		GSurfRamp.BaseColor ? 1 : 0, GSurfRoof.BaseColor ? 1 : 0,
		GMasterFloor ? 1 : 0, GMasterWall ? 1 : 0);
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
	// Structural always uses cohesion palette MIDs (not native warehouse mesh mats).
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

	ApplyProfileToMID(MID, SurfaceForType(Type));
}

EPFSurfaceRole RoleForPieceType(EPFPieceType Type)
{
	switch (Type)
	{
	case EPFPieceType::Wall:  return EPFSurfaceRole::WallConcrete;
	case EPFPieceType::Floor: return EPFSurfaceRole::FloorConcrete;
	case EPFPieceType::Ramp:  return EPFSurfaceRole::MetalRusty;
	case EPFPieceType::Roof:  return EPFSurfaceRole::MetalRoof;
	default:                  return EPFSurfaceRole::FloorConcrete;
	}
}

UMaterialInstanceDynamic* CreatePaletteMID(UObject* Outer, EPFSurfaceRole Role)
{
	EnsureLoaded();
	UMaterialInterface* Master = MasterForRole(Role);
	if (Master == nullptr)
	{
		// Absolute last resort — engine solid (ghost path still works via Color).
		Master = Cast<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")).TryLoad());
	}
	if (Master == nullptr)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Master, Outer);
	if (MID)
	{
		ApplyProfileToMID(MID, SurfaceForRole(Role));
	}
	return MID;
}

UMaterialInstanceDynamic* CreateStructuralPaletteMID(UObject* Outer, EPFPieceType Type)
{
	return CreatePaletteMID(Outer, RoleForPieceType(Type));
}

const TCHAR* StructuralSurfaceName(EPFPieceType Type)
{
	EnsureLoaded();
	return SurfaceForType(Type).Label;
}

const TCHAR* SurfaceRoleName(EPFSurfaceRole Role)
{
	switch (Role)
	{
	case EPFSurfaceRole::FloorConcrete: return TEXT("floor-concrete");
	case EPFSurfaceRole::WallConcrete:  return TEXT("wall-concrete");
	case EPFSurfaceRole::MetalRusty:    return TEXT("metal-rusty");
	case EPFSurfaceRole::MetalRoof:     return TEXT("metal-roof");
	default:                            return TEXT("?");
	}
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
