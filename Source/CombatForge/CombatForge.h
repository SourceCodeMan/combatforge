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
	constexpr int32 NetProtocol = 19;  // alpha.19 — 2026-08-06 playtest batch: kit-desync correction RPC,
	                                   // barrel-cooldown RPC, mantle latch in the saved moves, static bases.
	                                   // Burst tap-to-fire, sprint does not cancel reload, ramp underfill
	                                   // removed, shotgun recoil, local FP body arms (no gloved viewmodel
	                                   // stack), eye cam forward. No new RPCs — bump is ship discipline so
	                                   // clients and fleet never mix. PACKAGED 2026-08-06.
	                                   // (17 = alpha.17 — Pass-2 review minors. The one REPLICATION change is
	                                   // the new ServerSetFireMode RPC (P2-CB6). PACKAGED + itch-pushed
	                                   // 2026-08-06 as 0.1.0-alpha.17.)
	                                   // 16↔17 and 17↔18 must never mix.
	                                   // (16 = alpha.16, the 2026-07-24 batch: multi-server fleet (port-keyed
	                                   // directory + START NEW MATCH), forced Creative→Remix→RemixSwap
	                                   // cycle (GS CycleStage), phantom scrub (PS bHeadlessPhantom),
	                                   // per-player trap/one-way caps, PieceLimit deny reason, walkable
	                                   // cone, hold-Q wheel, eye-fallback fire origin, Pass-2 blocker+
	                                   // major fixes (bomb/frag damage-1 balls, kit clamps). Replicated
	                                   // layout changed in several places — 15↔16 must never mix.)
	                                   // (15 = alpha.15, FIRST-PERSON ARMS on top of everything in 14.)
	                                   // Carried from 14 (also never pushed): all 35 guns life-size +
	                                   // hand-tuned FP/ADS, computed TP grip + one-gun calibrate, left palm
	                                   // on the gun (humans; bots keep the tuned pose), midline tinted wall
	                                   // (opaque 1:30 then clears by 2:30), build 2:30, Yard bounds flush
	                                   // with the buildable pad, cone roof skin, code-review fix batch.
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
