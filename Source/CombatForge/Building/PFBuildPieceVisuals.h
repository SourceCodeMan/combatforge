// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/CombatForgeTypes.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;

/**
 * Shared build-piece display names, soft-loaded warehouse prop meshes, and
 * cohesion surface palette (warehouse concrete/metal textures on triplanar masters).
 *
 * Structural pieces (Wall/Floor/Ramp/Roof) keep engine basic-shape geometry so
 * edge placement / thin AABBs stay exact. Materials prefer Scene_Warehouse Surface
 * MIs (self-contained albedo — proven non-black). Fallback is M_PF_ArenaWall
 * triplanar + texture rebind. NEVER M_PF_ArenaFloor (miswired base color = pure black)
 * and NEVER M_PF_BuildPiece (ISM checker + local-edit guardrail).
 * Props soft-load Megascans from Scene_Warehouse (native materials).
 */
namespace PFBuildPieceVisuals
{
	/** Cohesion palette roles shared by arena shell + structural build pieces. */
	enum class EPFSurfaceRole : uint8
	{
		FloorConcrete,   // smooth warehouse floor
		WallConcrete,    // facade / bunker concrete (lighter tile than floor)
		MetalRusty,      // ramps, posts, trusses
		MetalRoof,       // painted roof / ceiling decks
		MAX
	};

	/** Display name for HUD / wheel (player-facing). */
	const TCHAR* DisplayName(EPFPieceType Type);
	const TCHAR* DisplayName(EPFBuildTool Tool);

	/**
	 * Ensure prop meshes + structural surface textures + triplanar masters are soft-loaded
	 * (idempotent, safe from game thread). Falls back to engine shapes / Concrete034.
	 */
	void EnsureLoaded();

	/** Mesh for a piece type (after EnsureLoaded). Never null if engine basic shapes exist. */
	UStaticMesh* MeshForType(EPFPieceType Type);

	/**
	 * World transform for a placed / ghost instance.
	 * Props use fitted scales so warehouse meshes sit in the same gameplay AABB as before.
	 * Structural types match FPFGridMath::PieceLocalTransform.
	 */
	FTransform PieceWorldTransform(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot);

	/** True when the mesh is a warehouse prop asset (native mats). Structural always false. */
	bool UsesNativeMaterials(EPFPieceType Type);

	/**
	 * Configure a structural (or ghost) MID for the given piece type:
	 * warehouse surface textures when present, tile size, AccentBoost.
	 * Call after SetVectorParameterValue("Color", ...). Safe no-op if MID is null.
	 */
	void ApplyStructuralSurface(UMaterialInstanceDynamic* MID, EPFPieceType Type);

	/** Map piece type → cohesion palette role. */
	EPFSurfaceRole RoleForPieceType(EPFPieceType Type);

	/**
	 * Create a MID on the shared triplanar master with warehouse textures for this role.
	 * Outer owns the MID. Returns null only if no master material could load.
	 * Never touches M_PF_BuildPiece.
	 */
	UMaterialInstanceDynamic* CreatePaletteMID(UObject* Outer, EPFSurfaceRole Role);

	/** Convenience: CreatePaletteMID(Outer, RoleForPieceType(Type)). */
	UMaterialInstanceDynamic* CreateStructuralPaletteMID(UObject* Outer, EPFPieceType Type);

	/** Short label for logging / debug (e.g. "wall-concrete", "ramp-metal"). */
	const TCHAR* StructuralSurfaceName(EPFPieceType Type);

	/** Role label for logging. */
	const TCHAR* SurfaceRoleName(EPFSurfaceRole Role);
}
