// Copyright (c) 2026 Tom Chapman. All rights reserved.
//
// Contract tests for FPFGridMath - the ONLY snap math in the game, shared by the placement
// ghost and the server's authoritative QueryPlacement. Everything here is pure (no world, no
// actors), so these are the cheapest real assertions in the project.
//
// They assert the CONTRACT (03 S2/S4 and the header's own docs) rather than mirroring the
// implementation. The load-bearing one is the level-cap sweep: APFBuildGrid::QueryPlacement
// rejects walls above MapLevels-1, ramps above MapLevels-2 and plates above MapLevels, so a snap
// function that can return a higher level hands the player a ghost the server always refuses.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Building/PFGridMath.h"
#include "Misc/AutomationTest.h"

namespace
{
	// The two shipped arena shells (Warehouse / The Yard), by level count.
	constexpr int32 ShippedLevelCounts[] = { PFGrid::Levels, PFGrid::YardLevels };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPFGridLevelCapTest,
	"CombatForge.Grid.SnapLevelsStayPlaceable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPFGridLevelCapTest::RunTest(const FString&)
{
	// Aim well below the floor and well above the roof: a player can point at either, and the
	// clamp is the only thing keeping the resulting record inside what the server will accept.
	for (const int32 NumLevels : ShippedLevelCounts)
	{
		const int32 MaxWall  = NumLevels - 1;   // QueryPlacement: PFIsWallLike && Level > MapLevels-1 -> reject
		const int32 MaxPlate = NumLevels;       // QueryPlacement: !bProp      && Level > MapLevels   -> reject

		for (int32 Z = -1200; Z <= NumLevels * PFGrid::WallHeightUU + 1200; Z += 25)
		{
			const FVector P(1000.0, 1600.0, static_cast<double>(Z));

			int32 Cx = 0, Cy = 0, WallLevel = 0;
			uint8 Edge = 0;
			FPFGridMath::SnapWall(P, Cx, Cy, WallLevel, Edge, NumLevels);
			if (WallLevel < 0 || WallLevel > MaxWall)
			{
				AddError(FString::Printf(
					TEXT("SnapWall(Z=%d, NumLevels=%d) returned level %d, outside 0..%d - the server would reject it"),
					Z, NumLevels, WallLevel, MaxWall));
				break;
			}
			TestTrue(TEXT("SnapWall always canonicalises to a N or E edge"), Edge <= 1);

			const int32 PlateLevel = FPFGridMath::WorldToCell(P, NumLevels).Z;
			if (PlateLevel < 0 || PlateLevel > MaxPlate)
			{
				AddError(FString::Printf(
					TEXT("WorldToCell(Z=%d, NumLevels=%d) returned level %d, outside 0..%d - the server would reject it"),
					Z, NumLevels, PlateLevel, MaxPlate));
				break;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPFGridWallCanonicalTest,
	"CombatForge.Grid.WallEdgeCanonicalisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPFGridWallCanonicalTest::RunTest(const FString&)
{
	// 03 S2: a wall has ONE canonical identity. Aiming at the S edge of cell (2,3) must resolve
	// to the N edge of (2,2), and the W edge to the E edge of (1,3) - otherwise two players
	// aiming at the same physical wall from opposite sides build two overlapping pieces.
	const double CellD = static_cast<double>(PFGrid::CellUU);
	struct FCase { double X; double Y; int32 Cx; int32 Cy; uint8 Edge; const TCHAR* What; };
	const FCase Cases[] = {
		{ 2.5 * CellD, 3.9 * CellD, 2, 3, 0, TEXT("N edge of (2,3) stays (2,3) N") },
		{ 2.5 * CellD, 3.1 * CellD, 2, 2, 0, TEXT("S edge of (2,3) becomes (2,2) N") },
		{ 2.9 * CellD, 3.5 * CellD, 2, 3, 1, TEXT("E edge of (2,3) stays (2,3) E") },
		{ 2.1 * CellD, 3.5 * CellD, 1, 3, 1, TEXT("W edge of (2,3) becomes (1,3) E") },
	};
	for (const FCase& C : Cases)
	{
		int32 Cx = 0, Cy = 0, Level = 0;
		uint8 Edge = 0;
		FPFGridMath::SnapWall(FVector(C.X, C.Y, 0.0), Cx, Cy, Level, Edge);
		TestEqual(FString::Printf(TEXT("%s (cell X)"), C.What), Cx, C.Cx);
		TestEqual(FString::Printf(TEXT("%s (cell Y)"), C.What), Cy, C.Cy);
		TestEqual(FString::Printf(TEXT("%s (edge)"),   C.What), static_cast<int32>(Edge), static_cast<int32>(C.Edge));
	}

	// Round-trip: the world position PieceLocalTransform puts a wall at must snap back to the
	// same cell + edge. Probed at the wall's BASE Z - the transform centres the wall half a
	// storey up, and rounding that to the nearest level legitimately lands on the level above.
	for (int32 Cx = -1; Cx <= 3; ++Cx)
	{
		for (int32 Cy = 0; Cy <= 3; ++Cy)
		{
			for (int32 L = 0; L < PFGrid::Levels; ++L)
			{
				for (uint8 Rot = 0; Rot <= 1; ++Rot)
				{
					const int16 X = static_cast<int16>(Cx * PFGrid::SubPerCell);
					const int16 Y = static_cast<int16>(Cy * PFGrid::SubPerCell);
					const int16 Z = static_cast<int16>(L * 3);
					const FVector Centre =
						FPFGridMath::PieceLocalTransform(EPFPieceType::Wall, X, Y, Z, Rot).GetLocation();
					const FVector Probe(Centre.X, Centre.Y, static_cast<double>(L * PFGrid::WallHeightUU));

					int32 BackX = 0, BackY = 0, BackL = 0;
					uint8 BackEdge = 0;
					FPFGridMath::SnapWall(Probe, BackX, BackY, BackL, BackEdge);
					if (BackX != Cx || BackY != Cy || BackL != L || BackEdge != Rot)
					{
						AddError(FString::Printf(
							TEXT("wall (%d,%d) level %d edge %u round-tripped to (%d,%d) level %d edge %u"),
							Cx, Cy, L, Rot, BackX, BackY, BackL, BackEdge));
						return true;
					}
				}
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPFGridQuantisationTest,
	"CombatForge.Grid.WorldQuantisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPFGridQuantisationTest::RunTest(const FString&)
{
	// "floor(P.xy/400)" - floor, not truncate. Truncation would fold the whole -400..0 strip
	// onto cell 0 and mirror one column of the field onto another.
	TestEqual(TEXT("x=10 -> cell 0"),    FPFGridMath::WorldToCell(FVector(10.0, 10.0, 0.0)).X, 0);
	TestEqual(TEXT("x=-10 -> cell -1"),  FPFGridMath::WorldToCell(FVector(-10.0, 10.0, 0.0)).X, -1);
	TestEqual(TEXT("x=-400 -> cell -1"), FPFGridMath::WorldToCell(FVector(-400.0, 10.0, 0.0)).X, -1);
	TestEqual(TEXT("x=-401 -> cell -2"), FPFGridMath::WorldToCell(FVector(-401.0, 10.0, 0.0)).X, -2);
	TestEqual(TEXT("x=399 -> cell 0"),   FPFGridMath::WorldToCell(FVector(399.0, 10.0, 0.0)).X, 0);
	TestEqual(TEXT("x=400 -> cell 1"),   FPFGridMath::WorldToCell(FVector(400.0, 10.0, 0.0)).X, 1);

	// "round(P/100)" for the 100 uu prop sub-grid.
	const FIntVector Sub = FPFGridMath::WorldToSubGrid(FVector(149.0, 151.0, -149.0));
	TestEqual(TEXT("sub-grid X rounds down"), Sub.X, 1);
	TestEqual(TEXT("sub-grid Y rounds up"),   Sub.Y, 2);
	TestEqual(TEXT("sub-grid Z rounds to -1"), Sub.Z, -1);

	// Ramp yaw: "camera yaw snapped to 90 degrees, plus the R offset", always 0..3.
	TestEqual(TEXT("yaw 0 -> +X"),      static_cast<int32>(FPFGridMath::RampYawFromCamera(0.f, 0)),   0);
	TestEqual(TEXT("yaw 90 -> +Y"),     static_cast<int32>(FPFGridMath::RampYawFromCamera(90.f, 0)),  1);
	TestEqual(TEXT("yaw 180 -> -X"),    static_cast<int32>(FPFGridMath::RampYawFromCamera(180.f, 0)), 2);
	TestEqual(TEXT("yaw 270 -> -Y"),    static_cast<int32>(FPFGridMath::RampYawFromCamera(270.f, 0)), 3);
	TestEqual(TEXT("yaw -90 wraps"),    static_cast<int32>(FPFGridMath::RampYawFromCamera(-90.f, 0)), 3);
	TestEqual(TEXT("yaw 359 wraps"),    static_cast<int32>(FPFGridMath::RampYawFromCamera(359.f, 0)), 0);
	TestEqual(TEXT("R offset rotates"), static_cast<int32>(FPFGridMath::RampYawFromCamera(0.f, 3)),   3);
	for (float Yaw = -720.f; Yaw <= 720.f; Yaw += 7.f)
	{
		for (uint8 Offset = 0; Offset <= 3; ++Offset)
		{
			const uint8 Step = FPFGridMath::RampYawFromCamera(Yaw, Offset);
			if (Step > 3)
			{
				AddError(FString::Printf(TEXT("RampYawFromCamera(%f, %u) returned %u, outside 0..3"),
					Yaw, Offset, Step));
				return true;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPFGridTeamPlotTest,
	"CombatForge.Grid.TeamPlotMembership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPFGridTeamPlotTest::RunTest(const FString&)
{
	int32 MinX = 0, MaxX = 0;
	TestTrue(TEXT("team 0 has a plot"), FPFGridMath::TeamPlotColumns(0, MinX, MaxX));
	TestTrue(TEXT("team 1 has a plot"), FPFGridMath::TeamPlotColumns(1, MinX, MaxX));
	TestFalse(TEXT("team 2 has no plot"), FPFGridMath::TeamPlotColumns(2, MinX, MaxX));
	TestFalse(TEXT("TeamNone (255) has no plot"), FPFGridMath::TeamPlotColumns(255, MinX, MaxX));

	// The two plots must not overlap, or one piece would be legal for both teams.
	int32 AMin = 0, AMax = 0, BMin = 0, BMax = 0;
	FPFGridMath::TeamPlotColumns(0, AMin, AMax);
	FPFGridMath::TeamPlotColumns(1, BMin, BMax);
	TestTrue(TEXT("team plots do not overlap"), AMax < BMin || BMax < AMin);

	// Rows outside the arena are never in a plot, whichever columns the team owns.
	TestTrue (TEXT("row 0 is in-bounds"),      FPFGridMath::IsCellInTeamPlot(AMin, 0, 0, PFGrid::CellsY));
	TestFalse(TEXT("row -1 is out of bounds"), FPFGridMath::IsCellInTeamPlot(AMin, -1, 0, PFGrid::CellsY));
	TestFalse(TEXT("row CellsY is out of bounds"),
		FPFGridMath::IsCellInTeamPlot(AMin, PFGrid::CellsY, 0, PFGrid::CellsY));
	TestFalse(TEXT("a column left of the plot is out"),
		FPFGridMath::IsCellInTeamPlot(AMin - 1, 0, 0, PFGrid::CellsY));
	TestFalse(TEXT("a column right of the plot is out"),
		FPFGridMath::IsCellInTeamPlot(AMax + 1, 0, 0, PFGrid::CellsY));
	return true;
}

#endif   // WITH_DEV_AUTOMATION_TESTS
