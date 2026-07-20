// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Building/PFBuildPieceVisuals.h"

#include "CombatForge.h"
#include "Building/PFGridMath.h"

#include "HAL/IConsoleManager.h"

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

	// GLoaded latches ONLY on a successful load (see EnsureLoaded). GLastAttemptFrame throttles
	// retries to one per frame — EnsureLoaded sits on hot paths (PieceWorldTransform runs per
	// instance), so an unthrottled retry loop after a miss would soft-load 20+ assets every call.
	bool GLoaded = false;
	uint64 GLastAttemptFrame = TNumericLimits<uint64>::Max();   // sentinel: never equals a real frame

	// The per-FRAME throttle above caps a miss at ~20 soft loads per frame — it does NOT cap the
	// number of frames. The build-phase ghost calls MeshForType + PieceWorldTransform EVERY tick
	// (PFBuildComponent::UpdateGhost), so on a client with a genuine cook gap that is ~2000 failed
	// TryLoads and ~2000 un-deduplicated engine "failed to find object" warnings PER SECOND for the
	// rest of the session — a permanent frame tax on the budget PCs we target, and a flooded log.
	// PFBuildGrid's retry timer giving up does not stop this: nothing else gates the ghost path.
	//
	// So bound it by WALL CLOCK, not by frames (frame count is meaningless across a 30fps laptop and
	// a 200fps desktop): retry every frame for GEagerWindowSeconds — that is the slow-client self-heal
	// this whole mechanism exists for, and it is 2x the grid timer's own ~15s give-up — then fall back
	// to one attempt every GCooldownSeconds. Deliberately a cooldown, not a hard stop, so content that
	// streams in very late still upgrades the ISMs in place; it just stops doing so at frame rate.
	constexpr double GEagerWindowSeconds = 30.0;
	constexpr double GCooldownSeconds = 30.0;
	double GFirstAttemptTime = 0.0;   // 0 = no attempt yet (FPlatformTime::Seconds() is never 0)
	double GLastAttemptTime = 0.0;
	bool GLoggedBackoff = false;      // the back-off notice prints exactly once per session

	// Test hook: force the first N EnsureLoaded attempts to resolve nothing, reproducing the
	// too-early-load race that a joining client hits for real. Lets the self-heal path be verified
	// deterministically instead of hoping to catch the race in a playtest. 0 = off (shipping).
	int32 GFailFirstN = 0;
	int32 GFailedSoFar = 0;
	FAutoConsoleVariableRef CVarPropVisualsFailFirstN(
		TEXT("pf.PropVisualsFailFirstN"),
		GFailFirstN,
		TEXT("DEBUG: make the first N build-piece asset loads fail, to test fallback recovery. 0=off."),
		ECVF_Cheat);
	FPropSlot GBarrel;   // PropCan
	FPropSlot GCrate;    // PropDorito
	FPropSlot GBoxes;    // PropSnake
	UStaticMesh* GCube = nullptr;
	UStaticMesh* GCylinder = nullptr;
	UStaticMesh* GCone = nullptr;

	// PREFERRED: warehouse Surface MIs (self-contained albedo — non-black).
	// FALLBACK: M_PF_ArenaWall triplanar only. NEVER assign M_PF_ArenaFloor (miswired → black).
	UMaterialInterface* GWhFloor = nullptr;
	UMaterialInterface* GWhMetal = nullptr;
	UMaterialInterface* GWhRoof = nullptr;
	UMaterialInterface* GMasterWall = nullptr;   // M_PF_ArenaWall (working)
	UMaterialInterface* GMasterFloor = nullptr;  // loaded for diagnostics only

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
			// Defensive cap: if a mesh's bounds haven't resolved yet (degenerate ~0 extent → MeshSize floored
			// to 1), FitScale would balloon and render as a giant block. A warehouse prop fit into a ~120–400 uu
			// footprint from a real-world-scale Megascan never legitimately needs >5×, so clamp per-axis. On a
			// properly-cooked, up-to-date client this never triggers (real FitScale is ~0.5–2×).
			Slot.FitScale.X = FMath::Min(Slot.FitScale.X, 5.f);
			Slot.FitScale.Y = FMath::Min(Slot.FitScale.Y, 5.f);
			Slot.FitScale.Z = FMath::Min(Slot.FitScale.Z, 5.f);
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

	/** Soft-loaded Megascans Surface MI for this role (nullptr if pack missing). */
	UMaterialInterface* WarehouseSurfaceForRole(EPFSurfaceRole Role)
	{
		switch (Role)
		{
		case EPFSurfaceRole::FloorConcrete:
		case EPFSurfaceRole::WallConcrete:
			// Same smooth floor MI for walls+floors: facade MI read near-black on engine cubes.
			return GWhFloor;
		case EPFSurfaceRole::MetalRusty:
			return GWhMetal;
		case EPFSurfaceRole::MetalRoof:
			return GWhRoof ? GWhRoof : GWhMetal;
		default:
			return GWhFloor;
		}
	}

	/** Triplanar fallback only — NEVER M_PF_ArenaFloor (black base-color graph). */
	UMaterialInterface* TriplanarFallbackMaster()
	{
		return GMasterWall;
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
	case EPFPieceType::Wall:           return TEXT("Wall");
	case EPFPieceType::Floor:          return TEXT("Floor");
	case EPFPieceType::Ramp:           return TEXT("Ramp");
	case EPFPieceType::Roof:           return TEXT("Ceiling");
	case EPFPieceType::PropCan:        return TEXT("Barrel");
	case EPFPieceType::PropDorito:     return TEXT("Crate");
	case EPFPieceType::PropSnake:      return TEXT("Boxes");
	case EPFPieceType::WallWindow:     return TEXT("Window");
	case EPFPieceType::WallDoor:       return TEXT("Door");
	case EPFPieceType::WallDoorOneWay: return TEXT("1-Way Door");
	case EPFPieceType::FloorTrap:      return TEXT("Trap Floor");
	default:                           return TEXT("?");
	}
}

const TCHAR* DisplayName(EPFBuildTool Tool)
{
	switch (Tool)
	{
	case EPFBuildTool::Wall:           return TEXT("Wall");
	case EPFBuildTool::Floor:          return TEXT("Floor");
	case EPFBuildTool::Ramp:           return TEXT("Ramp");
	case EPFBuildTool::Roof:           return TEXT("Ceiling");
	case EPFBuildTool::PropCan:        return TEXT("Barrel");
	case EPFBuildTool::PropDorito:     return TEXT("Crate");
	case EPFBuildTool::PropSnake:      return TEXT("Boxes");
	case EPFBuildTool::WallWindow:     return TEXT("Window");
	case EPFBuildTool::WallDoor:       return TEXT("Door");
	case EPFBuildTool::WallDoorOneWay: return TEXT("1-Way Door");
	case EPFBuildTool::FloorTrap:      return TEXT("Trap Floor");
	case EPFBuildTool::Delete:         return TEXT("Delete");
	default:                           return TEXT("?");
	}
}

void EnsureLoaded()
{
	if (GLoaded)
	{
		return;
	}

	// THE "giant checkered cone/cylinder/box" BUG (tasks #47, #89 — recurred through alpha-8).
	//
	// This used to set GLoaded = true on entry, so whatever the first attempt resolved was final. The
	// first attempt is NOT always at a safe moment: a joining client rebuilds the fort from the
	// FastArray, so PostReplicatedAdd -> AddPieceLocal -> EnsureLoaded can run inside net
	// serialization while the async loader owns the package. Every FSoftObjectPath::TryLoad below
	// then returns null, and the latch made that permanent for the whole session:
	//   - meshes  -> engine BasicShapes fallbacks (Cylinder / Cone / Cube at the legacy graybox
	//                scales in FPFGridMath::PieceLocalTransform) = the barrel/crate/box shapes
	//   - textures-> the triplanar master's texture params stay UNSET, and an unset Texture2D
	//                param samples the engine default, which is the gray CHECKERBOARD
	// One latch, both halves of the symptom. Never latch a miss: bail and let the next call retry.
	if (IsInAsyncLoadingThread() || IsGarbageCollecting())
	{
		return;
	}
	// Past the eager window this is a cook gap, not load contention — stop paying it every frame.
	// Without this the ghost path keeps EnsureLoaded at frame rate for the whole session (see the
	// note by GEagerWindowSeconds): ~2000 failed TryLoads + 2000 engine warnings per second.
	const double NowSeconds = FPlatformTime::Seconds();
	if (GFirstAttemptTime == 0.0)
	{
		GFirstAttemptTime = NowSeconds;
	}
	else if (NowSeconds - GFirstAttemptTime > GEagerWindowSeconds)
	{
		if (NowSeconds - GLastAttemptTime < GCooldownSeconds)
		{
			return;
		}
		if (!GLoggedBackoff)
		{
			GLoggedBackoff = true;
			UE_LOG(CombatForgeLog, Warning,
				TEXT("BuildPieceVisuals: content still unresolved after %.0fs — backing off to one attempt every %.0fs ")
				TEXT("(the build ghost was retrying every frame). Props stay on fallback shapes; check the cook."),
				GEagerWindowSeconds, GCooldownSeconds);
		}
	}

	if (GFrameCounter == GLastAttemptFrame)
	{
		return;   // already tried this frame — don't re-walk 20 soft paths per placed instance
	}
	GLastAttemptFrame = GFrameCounter;
	GLastAttemptTime = NowSeconds;

	if (GFailFirstN > 0 && GFailedSoFar < GFailFirstN)
	{
		++GFailedSoFar;
		UE_LOG(CombatForgeLog, Warning,
			TEXT("BuildPieceVisuals: SIMULATED load failure %d/%d (pf.PropVisualsFailFirstN)"),
			GFailedSoFar, GFailFirstN);
		return;   // nothing resolved, nothing latched — exactly the real race
	}

	// Engine fallbacks (always present). Soft-load so EnsureLoaded is safe outside constructors.
	GCube = SoftLoadMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	GCylinder = SoftLoadMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	GCone = SoftLoadMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));

	// ---- Structural surface profiles (materials; geometry stays basic shapes) ----
	InitSurfaceProfiles();

	// Warehouse Surface MIs first (working albedo). These are what the shell used successfully
	// before cohesion rebinding stomped them with broken M_PF_ArenaFloor MIDs.
	GWhFloor = Cast<UMaterialInterface>(FSoftObjectPath(
		TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Floor_Concrete_Smooth_01/MI_Ind_War_Floor_Concrete_Smooth_01_A.MI_Ind_War_Floor_Concrete_Smooth_01_A")).TryLoad());
	GWhMetal = Cast<UMaterialInterface>(FSoftObjectPath(
		TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Sheet_Metal_Rusty_01/MI_Ind_War_Sheet_Metal_Rusty_01_A.MI_Ind_War_Sheet_Metal_Rusty_01_A")).TryLoad());
	GWhRoof = Cast<UMaterialInterface>(FSoftObjectPath(
		TEXT("/Game/Scene_Warehouse/Assets/MS/Surfaces/Ind_War_Roof_Painted_01/MI_Ind_War_Roof_Painted_01.MI_Ind_War_Roof_Painted_01")).TryLoad());
	if (GWhRoof == nullptr)
	{
		GWhRoof = GWhMetal;
	}

	// Fallback triplanar: WALL only. M_PF_ArenaFloor base-color is miswired → pure black.
	GMasterWall = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaWall.M_PF_ArenaWall")).TryLoad());
	GMasterFloor = Cast<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ArenaFloor.M_PF_ArenaFloor")).TryLoad());

	// PropCan → metal / plastic barrel (upright cover).
	GBarrel.TargetSize = FVector(120.f, 120.f, 220.f);
	GBarrel.Paths[0] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Aba_Storage_Barrel_Metal_Blue_01/SM_Ind_Aba_Storage_Barrel_Metal_Blue_01.SM_Ind_Aba_Storage_Barrel_Metal_Blue_01");
	GBarrel.Paths[1] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_War_Storage_Barrel_Plastic_Blue_01/SM_Ind_War_Storage_Barrel_Plastic_Blue_01.SM_Ind_War_Storage_Barrel_Plastic_Blue_01");
	GBarrel.Paths[2] = TEXT("/Game/Scene_Warehouse/Assets/MS/3D/Ind_Sto_Barrel_Metal_Rust_03/SM_Ind_Sto_Barrel_Metal_Rust_03.SM_Ind_Sto_Barrel_Metal_Rust_03");
	FitSlot(GBarrel, GCylinder);

	// PropDorito → THE CONE. Restored 2026-07-20 at Tom's request: "the cone shape was working in early
	// versions, my first big play test and everyone liked it." It is also what the design has always
	// specified — docs/design/03-build-system.md: "Dorito | Wedge/tetra | r 120, h 200 | Cone
	// (2.4, 2.4, 2.0) | Mid cover, angled edges for lean-style peeks." Swapping it to a warehouse crate
	// was an art-pass decision that quietly dropped a piece players liked, and the angled faces are the
	// point: a box gives you square peeks, a cone gives you the lean-style ones the mode was built around.
	//
	// Deliberately NO warehouse paths. /Engine/BasicShapes/Cone ships with the engine itself, so this
	// piece is STRUCTURALLY IMMUNE to the whole "one player sees it, another doesn't" class of bug —
	// there is no /Game asset to miss from a cook, no soft path to lose a race against, nothing a client
	// can fail to have. Everything below (bWarehouse stays false) then routes it through
	// BasicShapeTransform, which is where the authored 2.4/2.4/2.0 cone transform already lives in
	// FPFGridMath::PieceLocalTransform — the geometry was never removed, only the look.
	GCrate.TargetSize = FVector(240.f, 240.f, 200.f);
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
	Keep(GWhFloor); Keep(GWhMetal); Keep(GWhRoof); Keep(GMasterWall); Keep(GMasterFloor);
	for (FSurfaceProfile* P : { &GSurfWall, &GSurfFloor, &GSurfRamp, &GSurfRoof, &GSurfFallback })
	{
		Keep(P->BaseColor); Keep(P->Normal); Keep(P->ORD);
	}

	// Latch only when the real content resolved. A partial result means we ran too early (see the
	// note at the top): keep GLoaded false so the next call — next frame, or the grid's retry timer —
	// tries again and upgrades the ISMs in place. Props falling back to engine shapes is the visible
	// failure; textures matter too (unset param = checkerboard).
	// GCrate is deliberately EXCLUDED: the Dorito is the engine Cone by design, so it can never report
	// bWarehouse. Requiring it here would leave GLoaded false forever — the retry timer would spin for
	// its full budget on every single client and then log "GAVE UP" on a load that was always complete.
	// Only slots that genuinely want a /Game asset belong in this test.
	GLoaded = GBarrel.bWarehouse && GBoxes.bWarehouse
		&& GSurfWall.bLoaded && GSurfFloor.bLoaded && GSurfRamp.bLoaded && GSurfRoof.bLoaded;

	UE_LOG(CombatForgeLog, Log,
		TEXT("BuildPieceVisuals: %s | props Barrel=%s%s Crate=%s%s Boxes=%s%s | tex wall=%d floor=%d ramp=%d roof=%d | warehouseMI floor=%d metal=%d roof=%d | triplanar wall=%d floorAsset=%d(black-do-not-use)"),
		GLoaded ? TEXT("READY") : TEXT("INCOMPLETE (will retry)"),
		GBarrel.Mesh ? *GBarrel.Mesh->GetName() : TEXT("null"), GBarrel.bWarehouse ? TEXT("") : TEXT("[FALLBACK]"),
		// "Dorito" not "Crate", and never [FALLBACK]: the cone is the intended mesh, not a failed load.
		// Tagging it as a fallback forever would be a diagnostic that cries wolf on every boot.
		GCrate.Mesh  ? *GCrate.Mesh->GetName()  : TEXT("null"), TEXT("(cone, by design)"),
		GBoxes.Mesh  ? *GBoxes.Mesh->GetName()  : TEXT("null"), GBoxes.bWarehouse  ? TEXT("") : TEXT("[FALLBACK]"),
		GSurfWall.bLoaded ? 1 : 0, GSurfFloor.bLoaded ? 1 : 0, GSurfRamp.bLoaded ? 1 : 0, GSurfRoof.bLoaded ? 1 : 0,
		GWhFloor ? 1 : 0, GWhMetal ? 1 : 0, GWhRoof ? 1 : 0,
		GMasterWall ? 1 : 0, GMasterFloor ? 1 : 0);
}

bool IsFullyLoaded()
{
	return GLoaded;
}

UStaticMesh* MeshForType(EPFPieceType Type)
{
	EnsureLoaded();
	if (const FPropSlot* Slot = SlotForProp(Type))
	{
		return Slot->Mesh;
	}
	// Structural pieces (Wall/Floor/Ramp/Roof + specials' ghosts) all use the unit cube;
	// per-type scale lives in FPFGridMath::PieceLocalTransform. Roof is a flat ceiling plate.
	return GCube;
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
	case EPFPieceType::Wall:
	case EPFPieceType::WallWindow:
	case EPFPieceType::WallDoor:
	case EPFPieceType::WallDoorOneWay:
		return EPFSurfaceRole::WallConcrete;
	case EPFPieceType::Floor:
	case EPFPieceType::FloorTrap:
		return EPFSurfaceRole::FloorConcrete;
	case EPFPieceType::Ramp:  return EPFSurfaceRole::MetalRusty;
	case EPFPieceType::Roof:  return EPFSurfaceRole::MetalRoof;
	default:                  return EPFSurfaceRole::FloorConcrete;
	}
}

UMaterialInstanceDynamic* CreatePaletteMID(UObject* Outer, EPFSurfaceRole Role)
{
	EnsureLoaded();

	// 1) Warehouse Surface MIs — preferred. Self-contained albedo; what worked before cohesion
	//    rebinding forced broken M_PF_ArenaFloor (black) onto every floor/wall cube.
	if (UMaterialInterface* Wh = WarehouseSurfaceForRole(Role))
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Wh, Outer);
		if (Mid)
		{
			return Mid;
		}
		// Some cooked MIs reject dynamic instances — assign via caller using parent is fine,
		// but we only return MIDs from this API; fall through to triplanar.
		UE_LOG(CombatForgeLog, Warning,
			TEXT("BuildPieceVisuals: Create MID from warehouse surface failed for role %s — triplanar fallback"),
			SurfaceRoleName(Role));
	}

	// 2) Working triplanar wall master + texture rebind (never ArenaFloor).
	//
	// Only if we actually HAVE textures to rebind. ApplyProfileToMID skips null textures, so a
	// triplanar MID built before the warehouse/CC0 textures resolve keeps the master's defaults —
	// and an unset Texture2D param samples the engine default texture, which is the gray
	// CHECKERBOARD players were seeing on barrels/crates/boxes. A flat BasicShapeMaterial gray
	// reads as untextured concrete instead: still wrong, but not alarming, and it self-corrects
	// on the retry once the real textures land.
	const FSurfaceProfile& Profile = SurfaceForRole(Role);
	UMaterialInterface* Master = Profile.bLoaded ? TriplanarFallbackMaster() : nullptr;
	if (Master == nullptr)
	{
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
		ApplyProfileToMID(MID, Profile);
		if (!Profile.bLoaded)
		{
			// BasicShapeMaterial's full-surface tint — neutral concrete-ish gray, no checker.
			MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.42f, 0.42f, 0.40f));
		}
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
