// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Building/PFArenaShell.h"
#include "PFYardShell.generated.h"

/**
 * "THE YARD" v2 (task #44, Tom's spec): a WIDE-OPEN desert field. Same 6400 length, DOUBLE
 * width (8000), open air, and — unlike the Warehouse box — NO perimeter walls, NO escape lid
 * (bPerimeter=false in the def gates them out of the base ctor). The concrete pad sits in the
 * open: you can simply walk off it onto the sand. The warehouse building stands next door
 * along the north edge, and a giant sand plane + distant mesas sell the desert horizon.
 *
 * Build-phase halves stay honest because the base ctor extends the invisible midline barrier
 * 20000 uu into the desert on open-field maps (you can't stroll around it).
 *
 * Desert parts are deliberately NOT registered in DressingParts — the cohesion palette pass
 * would rebind them to warehouse concrete. They keep their own sand-tinted MIDs (BeginPlay;
 * BasicShapeMaterial "Color" param — the proven tintable engine material).
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

protected:
	virtual void BeginPlay() override;

private:
	/** Cube-primitive warehouse building north of the field (ctor-time; MakeShapePart parts). */
	void BuildWarehouseFacade();
	/** Sand ground plane + distant mesa/dune forms (ctor-time; kept OUT of DressingParts). */
	void BuildDesert();

	/** /Engine/BasicShapes/BasicShapeMaterial — hard CDO ref (reliable for /Engine content). */
	UPROPERTY() TObjectPtr<UMaterialInterface> SandBaseMaterial;
	/** Desert parts awaiting their sand MIDs (index-parallel with SandTints). */
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> SandParts;
	TArray<FLinearColor> SandTints;
};
