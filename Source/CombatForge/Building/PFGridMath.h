// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Core/CombatForgeTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"

/**
 * Header-only pure grid math (contract §3.5 / 03 §1-§4). All statics, no state — the ONLY snap
 * math in the game. Field-local space == world space (field corner at world origin, 03 §1).
 *
 * Record coordinate semantics (FPFBuildPieceRec / FPFPlacementQuery):
 *  - X/Y/Z are sub-grid units (100 uu).
 *  - Structural: X/Y = cell min-corner (multiples of 4), Z = level*3 (0/3/6/… up to map Levels).
 *  - Props: X/Y = center, Z = support-top (multiples of 3 — floor tops are 300-multiples, T25).
 *  - Rot: 0-3 = 90° yaw steps. Walls: canonical edge 0=N (+Y edge), 1=E (+X edge).
 *    Ramps: index of the ascent direction (0=+X, 1=+Y, 2=-X, 3=-Y); low edge nearest the player.
 */
struct COMBATFORGE_API FPFGridMath
{
	/** 300 rise / 400 run — Fortnite's exact slope on a 3-4-5 plank (B4). */
	static constexpr float RampPitchDeg = 36.86989765f;

	// ---------------------------------------------------------------
	// World → grid quantization (03 §4)
	// ---------------------------------------------------------------

	/** Cell + level from a world point: floor(P.xy/400), level = clamp(round(P.z/300), 0, NumLevels). */
	static FIntVector WorldToCell(const FVector& P, int32 NumLevels = PFGrid::Levels)
	{
		const int32 Cx = FMath::FloorToInt32(P.X / static_cast<double>(PFGrid::CellUU));
		const int32 Cy = FMath::FloorToInt32(P.Y / static_cast<double>(PFGrid::CellUU));
		const int32 MaxLevel = FMath::Max(0, NumLevels);
		const int32 Level = FMath::Clamp(
			FMath::RoundToInt32(P.Z / static_cast<double>(PFGrid::WallHeightUU)), 0, MaxLevel);
		return FIntVector(Cx, Cy, Level);
	}

	/** Nearest 100 uu sub-grid point: round(P/100). */
	static FIntVector WorldToSubGrid(const FVector& P)
	{
		return FIntVector(
			FMath::RoundToInt32(P.X / static_cast<double>(PFGrid::SubUU)),
			FMath::RoundToInt32(P.Y / static_cast<double>(PFGrid::SubUU)),
			FMath::RoundToInt32(P.Z / static_cast<double>(PFGrid::SubUU)));
	}

	/**
	 * Wall snap: nearest of the 4 cell edges from frac(P.xy/400), canonicalized S/W → the
	 * neighbor cell's N/E (03 §2). Level clamps to 0..NumLevels-1 — top-base walls are legal
	 * and crown flush at the cap. Query still rejects a wall at Level == MapLevels.
	 * EdgeNE: 0 = N (+Y edge of the anchor cell), 1 = E (+X edge).
	 */
	static void SnapWall(const FVector& P, int32& CellX, int32& CellY, int32& Level, uint8& EdgeNE,
	                     int32 NumLevels = PFGrid::Levels)
	{
		CellX = FMath::FloorToInt32(P.X / static_cast<double>(PFGrid::CellUU));
		CellY = FMath::FloorToInt32(P.Y / static_cast<double>(PFGrid::CellUU));
		const float FracX = static_cast<float>(P.X / PFGrid::CellUU - static_cast<double>(CellX));
		const float FracY = static_cast<float>(P.Y / PFGrid::CellUU - static_cast<double>(CellY));

		// Distance from the anchor point to each edge midline; deterministic tie order N,E,S,W.
		uint8 Edge = 0;                       // 0=N, 1=E, 2=S, 3=W (local, pre-canonical)
		float Best = 1.f - FracY;             // N
		if (1.f - FracX < Best) { Best = 1.f - FracX; Edge = 1; }   // E
		if (FracY < Best)       { Best = FracY;       Edge = 2; }   // S
		if (FracX < Best)       { Best = FracX;       Edge = 3; }   // W

		if (Edge == 2)      { CellY -= 1; Edge = 0; }   // S → neighbor's N
		else if (Edge == 3) { CellX -= 1; Edge = 1; }   // W → neighbor's E
		EdgeNE = Edge;

		const int32 MaxWallLevel = FMath::Max(0, NumLevels - 1);
		Level = FMath::Clamp(
			FMath::RoundToInt32(P.Z / static_cast<double>(PFGrid::WallHeightUU)), 0, MaxWallLevel);
	}

	/**
	 * Ramp rotation from the camera: ascent direction = camera yaw snapped to 90° (low edge
	 * nearest the player — Fortnite default) plus the R offset. Returns 0..3.
	 */
	static uint8 RampYawFromCamera(float CameraYawDeg, uint8 ROffset)
	{
		const float Norm = FRotator::ClampAxis(CameraYawDeg);            // 0..360
		const int32 Step = FMath::RoundToInt32(Norm / 90.f) % 4;         // 0=+X, 1=+Y, 2=-X, 3=-Y
		return static_cast<uint8>((Step + ROffset) % 4);
	}

	// ---------------------------------------------------------------
	// Grid → world (single source of truth for ISM instances AND the ghost)
	// ---------------------------------------------------------------

	/**
	 * Full render/collision transform (location, rotation, component scale on the 100 uu engine
	 * primitives) for a piece record. Geometry per contract §4.2.
	 */
	static FTransform PieceLocalTransform(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot)
	{
		const float S = static_cast<float>(PFGrid::SubUU);   // 100 uu
		const float Wx = X * S;
		const float Wy = Y * S;
		const float Wz = Z * S;                              // structural level base / prop support top

		switch (Type)
		{
		case EPFPieceType::Wall:
		case EPFPieceType::WallWindow:
		case EPFPieceType::WallDoor:
		case EPFPieceType::WallDoorOneWay:
			// Cube (4.0, 0.2, 3.0) = 400×20×300 on the canonical edge, base at level Z.
			// Special walls use the same ghost footprint; runtime actors own the real collision.
			if (Rot == 0)   // N edge: runs along X at y = corner + 400
			{
				return FTransform(FRotator::ZeroRotator,
					FVector(Wx + 200.f, Wy + 400.f, Wz + 150.f), FVector(4.f, 0.2f, 3.f));
			}
			// E edge: runs along Y at x = corner + 400
			return FTransform(FRotator(0.f, 90.f, 0.f),
				FVector(Wx + 400.f, Wy + 200.f, Wz + 150.f), FVector(4.f, 0.2f, 3.f));

		case EPFPieceType::Floor:
		case EPFPieceType::FloorTrap:
			// Cube (4.0, 4.0, 0.2) = 400×400×20, TOP at level Z (T25) → center 10 below.
			return FTransform(FRotator::ZeroRotator,
				FVector(Wx + 200.f, Wy + 200.f, Wz - 10.f), FVector(4.f, 4.f, 0.2f));

		case EPFPieceType::Ramp:
			// Cube plank (5.0, 4.0, 0.2) = 500×400×20 pitched to 36.87°; positive pitch raises the
			// plank's local +X, so it ascends along the Rot direction with the low edge at level base.
			return FTransform(FRotator(RampPitchDeg, Rot * 90.f, 0.f),
				FVector(Wx + 200.f, Wy + 200.f, Wz + 150.f), FVector(5.f, 4.f, 0.2f));

		case EPFPieceType::Roof:
			// Flat ceiling plate (same footprint as Floor) — replaces the old cone graybox.
			// Cube (4.0, 4.0, 0.2) = 400×400×20, top at level Z.
			return FTransform(FRotator::ZeroRotator,
				FVector(Wx + 200.f, Wy + 200.f, Wz - 10.f), FVector(4.f, 4.f, 0.2f));

		case EPFPieceType::PropCan:
			// Cylinder (1.2, 1.2, 2.2) = r60 h220, resting on the support top.
			return FTransform(FRotator(0.f, Rot * 90.f, 0.f),
				FVector(Wx, Wy, Wz + 110.f), FVector(1.2f, 1.2f, 2.2f));

		case EPFPieceType::PropDorito:
			// Cone (2.4, 2.4, 1.0) = r120 h100. Height halved from the original h200 (Tom 2026-07-24):
			// players must be able to JUMP ON and WALK ON the cone. Slope atan(100/120) = 39.8°, under
			// CharacterMovement's default 44.765° walkable limit; at h200 the 59° face shed the player.
			return FTransform(FRotator(0.f, Rot * 90.f, 0.f),
				FVector(Wx, Wy, Wz + 50.f), FVector(2.4f, 2.4f, 1.f));

		case EPFPieceType::PropSnake:
			// Cube (4.0, 1.2, 1.2) = 400×120×120.
			return FTransform(FRotator(0.f, Rot * 90.f, 0.f),
				FVector(Wx, Wy, Wz + 60.f), FVector(4.f, 1.2f, 1.2f));

		default:
			return FTransform::Identity;
		}
	}

	/** World-space AABB of a piece record (inputs in sub-grid ints). False on invalid type/rot. */
	static bool PieceAABB(EPFPieceType Type, int16 X, int16 Y, int16 Z, uint8 Rot, FBox& Out)
	{
		const float S = static_cast<float>(PFGrid::SubUU);
		const float Wx = X * S;
		const float Wy = Y * S;
		const float Wz = Z * S;

		switch (Type)
		{
		case EPFPieceType::Wall:
		case EPFPieceType::WallWindow:
		case EPFPieceType::WallDoor:
		case EPFPieceType::WallDoorOneWay:
			if (Rot > 1)
			{
				return false;
			}
			if (Rot == 0)
			{
				Out = FBox(FVector(Wx, Wy + 390.f, Wz), FVector(Wx + 400.f, Wy + 410.f, Wz + 300.f));
			}
			else
			{
				Out = FBox(FVector(Wx + 390.f, Wy, Wz), FVector(Wx + 410.f, Wy + 400.f, Wz + 300.f));
			}
			return true;

		case EPFPieceType::Floor:
		case EPFPieceType::FloorTrap:
			Out = FBox(FVector(Wx, Wy, Wz - 20.f), FVector(Wx + 400.f, Wy + 400.f, Wz));
			return true;

		case EPFPieceType::Ramp:
			Out = FBox(FVector(Wx, Wy, Wz), FVector(Wx + 400.f, Wy + 400.f, Wz + 300.f));
			return true;

		case EPFPieceType::Roof:
			// Flat ceiling plate — same AABB as Floor (top at level Z).
			Out = FBox(FVector(Wx, Wy, Wz - 20.f), FVector(Wx + 400.f, Wy + 400.f, Wz));
			return true;

		case EPFPieceType::PropCan:
			Out = FBox(FVector(Wx - 60.f, Wy - 60.f, Wz), FVector(Wx + 60.f, Wy + 60.f, Wz + 220.f));
			return true;

		case EPFPieceType::PropDorito:
			Out = FBox(FVector(Wx - 120.f, Wy - 120.f, Wz), FVector(Wx + 120.f, Wy + 120.f, Wz + 100.f));
			return true;

		case EPFPieceType::PropSnake:
			if ((Rot % 2) == 0)   // long axis along X
			{
				Out = FBox(FVector(Wx - 200.f, Wy - 60.f, Wz), FVector(Wx + 200.f, Wy + 60.f, Wz + 120.f));
			}
			else                  // long axis along Y
			{
				Out = FBox(FVector(Wx - 60.f, Wy - 200.f, Wz), FVector(Wx + 60.f, Wy + 200.f, Wz + 120.f));
			}
			return true;

		default:
			return false;
		}
	}

	// ---------------------------------------------------------------
	// Plot membership (PFGrid columns, 03 §5)
	// ---------------------------------------------------------------

	/** Cell column range of a team's build plot. False for Team > 1. */
	static bool TeamPlotColumns(uint8 Team, int32& OutMinX, int32& OutMaxX)
	{
		if (Team == 0) { OutMinX = PFGrid::PlotAMinX; OutMaxX = PFGrid::PlotAMaxX; return true; }
		if (Team == 1) { OutMinX = PFGrid::PlotBMinX; OutMaxX = PFGrid::PlotBMaxX; return true; }
		return false;
	}

	/** True iff cell (Cx, Cy) lies inside the given team's build plot. */
	static bool IsCellInTeamPlot(int32 Cx, int32 Cy, uint8 Team, int32 CellsY)
	{
		if (Cy < 0 || Cy >= CellsY)
		{
			return false;
		}
		int32 MinX = 0, MaxX = 0;
		if (!TeamPlotColumns(Team, MinX, MaxX))
		{
			return false;
		}
		return Cx >= MinX && Cx <= MaxX;
	}

	/**
	 * Plot membership per record coordinates. Structural: the anchor cell must be in the plot.
	 * Props: the center must be in the plot rectangle.
	 * CONTRACT-GAP: the contracted signature carries no Rot, so the rot-aware prop AABB-in-plot
	 * check and the wall edge-adjacency ruling live in APFBuildGrid::QueryPlacement (the one
	 * predicate both ghost and server run); this function is the coarse per-record test.
	 */
	static bool IsInsideTeamPlot(EPFPieceType Type, int16 X, int16 Y, uint8 Team, int32 CellsY)
	{
		if (!PFIsProp(Type))
		{
			return IsCellInTeamPlot(X / PFGrid::SubPerCell, Y / PFGrid::SubPerCell, Team, CellsY);
		}
		int32 MinC = 0, MaxC = 0;
		if (!TeamPlotColumns(Team, MinC, MaxC))
		{
			return false;
		}
		const float Px = X * static_cast<float>(PFGrid::SubUU);
		const float Py = Y * static_cast<float>(PFGrid::SubUU);
		return Px >= MinC * PFGrid::CellUU && Px <= (MaxC + 1) * PFGrid::CellUU
			&& Py >= 0.f && Py <= static_cast<float>(CellsY * PFGrid::CellUU);
	}

	// ---------------------------------------------------------------
	// Prop support height
	// ---------------------------------------------------------------

	/**
	 * Sub-grid Z of the surface supporting a prop at this XY: terrain (0) or the highest
	 * blocking support under the height cap. Floor tops are exact 300-multiples (T25), so the
	 * result is quantized to level tops (0/3/6/… sub-grid). Traces PF_ECC_BuildTrace downward.
	 */
	static int16 SupportTopSubZ(const UWorld* World, const FVector& XY,
	                            int32 NumLevels = PFGrid::Levels,
	                            int32 HeightCapUU = PFGrid::HeightCapUU)
	{
		if (!World)
		{
			return 0;
		}
		const FVector Start(XY.X, XY.Y, static_cast<float>(HeightCapUU) + 100.f);
		const FVector End(XY.X, XY.Y, -50.f);
		FCollisionQueryParams Params;
		Params.bTraceComplex = false;

		// Supports are terrain and FLOOR TOPS only (T25), and those sit at exact 300-multiples.
		// A single trace took whatever it hit first, so standing beside a barrel (top ~220 uu)
		// quantized the ghost to level 1 and the piece then failed NoAnchor or flickered invalid.
		// Multi-trace and take the highest hit that actually lands on a level top; props and other
		// arbitrary-height geometry are skipped rather than rounded to the nearest floor. (P2-BD4)
		TArray<FHitResult> Hits;
		if (!World->LineTraceMultiByChannel(Hits, Start, End, PF_ECC_BuildTrace, Params))
		{
			return 0;
		}
		// NumLevels-1, deliberately ONE LOWER than WorldToCell's plate cap. A plate is legal at
		// Level == NumLevels (it crowns flush at HeightCap), but a prop standing on that plate puts
		// its own AABB 100-220 uu ABOVE the cap and QueryPlacement's height test always denies it.
		// Snapping the ghost up there would only ever preview an unplaceable piece, so a hit on the
		// top plate is skipped and the search keeps falling to the highest support a prop can use.
		const int32 MaxLevel = FMath::Max(0, NumLevels - 1);
		constexpr double LevelTopToleranceUU = 8.0;   // same slack the height-cap check uses
		for (const FHitResult& Hit : Hits)   // ordered from Start (highest) downward
		{
			const double Z = Hit.ImpactPoint.Z;
			const int32 Level = FMath::RoundToInt32(Z / static_cast<double>(PFGrid::WallHeightUU));
			if (Level < 0 || Level > MaxLevel)
			{
				continue;
			}
			if (FMath::Abs(Z - static_cast<double>(Level) * PFGrid::WallHeightUU) > LevelTopToleranceUU)
			{
				continue;   // a prop / sloped surface, not a floor top
			}
			return static_cast<int16>(Level * 3);
		}
		return 0;   // nothing but props under us — ground level
	}
};
