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
	constexpr int32 NetProtocol = 17;  // alpha.17 — Pass-2 review minors. The one REPLICATION change is
	                                   // the new ServerSetFireMode RPC (P2-CB6): the server now knows the
	                                   // owning client's fire selector and enforces a human-tap sustained
	                                   // cadence on Single/Burst, so a modified client can no longer run a
	                                   // Mode_S weapon at its internal ROF. Everything else in the batch is
	                                   // client-side or server-only, but the RPC alone means 16↔17 must
	                                   // never mix. PACKAGED + itch-pushed 2026-08-06 (windows-alpha
	                                   // 0.1.0-alpha.17). Fleet must redeploy the same binary.
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
