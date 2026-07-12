// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PaintForgeTypes.h"

class UStaticMesh;

/**
 * Shared build-piece display names + soft-loaded warehouse prop meshes.
 * Structural pieces (Wall/Floor/Ramp/Roof) stay engine basic shapes.
 * Props soft-load Megascans from Scene_Warehouse so the CDO never freezes on compile.
 */
namespace PFBuildPieceVisuals
{
	/** Display name for HUD / wheel (player-facing). */
	const TCHAR* DisplayName(EPFPieceType Type);
	const TCHAR* DisplayName(EPFBuildTool Tool);

	/**
	 * Ensure prop static meshes are soft-loaded (idempotent, safe from game thread).
	 * Falls back to engine Cylinder/Cone/Cube if warehouse assets are missing.
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

	/** True when the mesh is a warehouse (or similar) asset rather than an engine basic shape. */
	bool UsesNativeMaterials(EPFPieceType Type);
}
