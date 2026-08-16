// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "CombatForge.h"

#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(CombatForgeLog);

bool PFIsRenderingWorld(const UWorld* World)
{
	return World != nullptr && World->GetNetMode() != NM_DedicatedServer;
}

void FCombatForgeModule::StartupModule()
{
	FDefaultGameModuleImpl::StartupModule();

	// Skip the content probe in commandlet contexts (build tooling); the cooker follows the
	// CDO FObjectFinder references regardless (02 D10). Everywhere a player runs, the checks
	// below must pass.
	if (IsRunningCommandlet())
	{
		UE_LOG(CombatForgeLog, Log, TEXT("CombatForge module started (commandlet - engine asset checks skipped)"));
		return;
	}

	// The four engine primitives every graybox mesh in the game is built from (02 §3.3, §4.2).
	static const TCHAR* MeshPaths[] =
	{
		TEXT("/Engine/BasicShapes/Cube.Cube"),
		TEXT("/Engine/BasicShapes/Sphere.Sphere"),
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
		TEXT("/Engine/BasicShapes/Cone.Cone")
	};

	for (const TCHAR* Path : MeshPaths)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Path);
		ensureMsgf(Mesh != nullptr, TEXT("CombatForge: engine basic shape missing: %s"), Path);
	}

	// The one safely MID-tintable engine material; its "Color" vector parameter is the only
	// tint knob in the whole game (02 §3.3).
	UMaterialInterface* BasicShapeMaterial = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (ensureMsgf(BasicShapeMaterial != nullptr,
		TEXT("CombatForge: /Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial is missing")))
	{
		FLinearColor Unused;
		const bool bHasColorParam = BasicShapeMaterial->GetVectorParameterValue(
			FHashedMaterialParameterInfo(TEXT("Color")), Unused);
		ensureMsgf(bHasColorParam,
			TEXT("CombatForge: BasicShapeMaterial no longer exposes a 'Color' vector parameter - team tinting is broken"));
	}

	UE_LOG(CombatForgeLog, Log, TEXT("CombatForge module started (engine asset checks done)"));
}

void FCombatForgeModule::ShutdownModule()
{
	UE_LOG(CombatForgeLog, Log, TEXT("CombatForge module shut down"));
	FDefaultGameModuleImpl::ShutdownModule();
}

IMPLEMENT_PRIMARY_GAME_MODULE(FCombatForgeModule, CombatForge, "CombatForge");
