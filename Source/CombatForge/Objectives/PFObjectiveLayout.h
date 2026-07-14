// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/CombatForgeTypes.h"

/**
 * Objective world positions derived solely from PFGrid field constants.
 * No PFArenaShell methods — arena geometry ownership stays on the warehouse lane.
 * Field-local == world (field corner at origin). Z sits slightly above the floor slab.
 */
namespace PFObjectiveLayout
{
	/**
	 * World Z for objective actor pivots sitting on the Z=0 floor slab.
	 * Engine Cylinder (h=100) scaled Z=0.15 → height 15 uu → center at 7.5.
	 * Previous FloorZ=50 left Dom/HP pads floating mid-air.
	 */
	constexpr float FloorZ = 8.f;

	/** Center of cell (Cx, Cy) at floor height. */
	inline FVector CellCenter(int32 Cx, int32 Cy, float Z = FloorZ)
	{
		return FVector(
			(static_cast<float>(Cx) + 0.5f) * static_cast<float>(PFGrid::CellUU),
			(static_cast<float>(Cy) + 0.5f) * static_cast<float>(PFGrid::CellUU),
			Z);
	}

	/** CTF flag homes: team spawn column mid-Y (near each team's spawn strip). */
	inline FVector FlagHome(uint8 Team)
	{
		const int32 MidY = PFGrid::CellsY / 2;
		if (Team == 0)
		{
			return CellCenter(PFGrid::SpawnColA, MidY);
		}
		return CellCenter(PFGrid::SpawnColB, MidY);
	}

	/** Domination / Hardpoint slot count (A-plot, mid, B-plot). */
	constexpr int32 ControlPointCount = 3;

	/**
	 * Control-point world positions:
	 *  0 = A-plot mid cell, 1 = neutral mid, 2 = B-plot mid. All at field mid-Y.
	 */
	inline FVector ControlPointLocation(int32 Index)
	{
		const int32 MidY = PFGrid::CellsY / 2;
		const int32 Clamped = FMath::Clamp(Index, 0, ControlPointCount - 1);
		switch (Clamped)
		{
		case 0:
			return CellCenter((PFGrid::PlotAMinX + PFGrid::PlotAMaxX) / 2, MidY);
		case 1:
			return CellCenter((PFGrid::NeutralMinX + PFGrid::NeutralMaxX) / 2, MidY);
		default:
			return CellCenter((PFGrid::PlotBMinX + PFGrid::PlotBMaxX) / 2, MidY);
		}
	}
}
