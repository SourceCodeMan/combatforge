// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Building/PFArenaShell.h"
#include "PFYardShell.generated.h"

/**
 * "THE YARD" (task #40): same 6400 length, DOUBLE width (8000), open air. The base ctor builds
 * the entire functional field from PFGetArenaMapDef(Yard) — floor, perimeter, escape lid,
 * full-width spawn strips / midline / barrier, and the unchanged south warm-up pen — so this
 * subclass adds scenery only: a NON-ENTERABLE warehouse facade along the NORTH long edge (the
 * pen owns the south exterior at Y=-3000), selling "playing in the yard beside the warehouse".
 *
 * The facade sits outside the perimeter wall, which already blocks entry — so every facade part
 * is Cosmetic (NoCollision), with bCastShadow on the big slabs so the sun (pitch -52°) throws the
 * building's shadow onto the field. Sun runs at 4.0 here (def) — open-air at the Warehouse's 6.0
 * would blow the floor out with exposure locked (UPFLightingSubsystem::ConfigureForMap).
 *
 * Map identity travels as ACTOR CLASS: the GameMode spawns this class and ordinary actor-channel
 * replication makes every client construct the identical ctor-built geometry (base-class contract).
 */
UCLASS()
class COMBATFORGE_API APFYardShell : public APFArenaShell
{
	GENERATED_BODY()

public:
	APFYardShell();

private:
	/** Cube-primitive warehouse building north of the field (ctor-time; MakeShapePart parts). */
	void BuildWarehouseFacade();
};
