// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * The one log category for the whole game (contract §3.1). Every package logs through
 * UE_LOG(PaintForgeLog, ...). Verbosity conventions (§4.1):
 *   Log     = state transitions
 *   Verbose = per-shot / per-placement
 *   Warning = validation rejects
 *   Error   = contract violations
 */
DECLARE_LOG_CATEGORY_EXTERN(PaintForgeLog, Log, All);

/**
 * Primary game module. StartupModule verifies the engine BasicShapes assets and the
 * BasicShapeMaterial "Color" vector parameter exist (02 §3.3) so an engine content rename
 * fails loudly at boot instead of silently as white-on-white graybox.
 */
class FPaintForgeModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
