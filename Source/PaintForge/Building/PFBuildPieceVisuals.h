// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PaintForgeTypes.h"

class UMaterialInstanceDynamic;
class UStaticMesh;

/**
 * Shared build-piece display names, soft-loaded warehouse prop meshes, and
 * per-type structural surface profiles (triplanar textures + team accent).
 *
 * Structural pieces (Wall/Floor/Ramp/Roof) keep engine basic-shape geometry so
 * edge placement / thin AABBs stay exact; art comes from M_PF_BuildPiece MIDs
 * with warehouse (or Concrete034) textures applied per type.
 * Props soft-load Megascans from Scene_Warehouse (native materials).
 */
namespace PFBuildPieceVisuals
{
	/** Display name for HUD / wheel (player-facing). */
	const TCHAR* DisplayName(EPFPieceType Type);
	const TCHAR* DisplayName(EPFBuildTool Tool);

	/**
	 * Ensure prop meshes + structural surface textures are soft-loaded
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

	/** Short label for logging / debug (e.g. "wall-concrete", "ramp-metal"). */
	const TCHAR* StructuralSurfaceName(EPFPieceType Type);
}
