// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * The one log category for the whole game (contract §3.1). Every package logs through
 * UE_LOG(CombatForgeLog, ...). Verbosity conventions (§4.1):
 *   Log     = state transitions
 *   Verbose = per-shot / per-placement
 *   Warning = validation rejects
 *   Error   = contract violations
 */
DECLARE_LOG_CATEGORY_EXTERN(CombatForgeLog, Log, All);

/**
 * Multiplayer build gate. This number is folded into the network version (UCombatForgeGameInstance::Init),
 * so two clients on different CombatForge builds fail the join handshake with a clean version-mismatch
 * error instead of connecting and silently mis-rendering (stale build = giant boxes / missing skins).
 * ⚠️ BUMP THIS ON EVERY itch.io PACKAGE/PUSH (keep it in step with the alpha userversion, e.g. alpha.4 → 4).
 */
namespace PFBuild
{
	constexpr int32 NetProtocol = 7;   // alpha.7 — ServerSetPlayerName RPC (account name in game) + forces all
	                                   // clients onto matching content (fixes old-build black-checker props)
}

/**
 * Primary game module. StartupModule verifies the engine BasicShapes assets and the
 * BasicShapeMaterial "Color" vector parameter exist (02 §3.3) so an engine content rename
 * fails loudly at boot instead of silently as white-on-white graybox.
 */
class FCombatForgeModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
