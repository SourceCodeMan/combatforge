// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PFCombatAudio.generated.h"

/**
 * Combat audio stub hooks (04 §4, contract §3.4).
 *
 * v1 ships zero audio assets: every function is a named no-op that logs at Verbose so
 * the mix can be traced in PIE. THE CALL SITES ARE THE DELIVERABLE — the polish pass
 * replaces these bodies with real one-shots without touching any caller.
 *
 * Call-site map (who calls what):
 *   PlayHitmarker()     — UPFCombatFeedbackWidget on hit confirm
 *   PlayElim()          — UPFCombatFeedbackWidget on own elim confirm
 *   PlayMuzzle()        — UPFWeaponComponent, per shot (local + remote cosmetic)
 *   PlaySplatIncoming() — UPFHealthComponent, owning client on paint hit taken
 *   PlayBreakout()      — GameMode round-start horn (pkg-core)
 *   PlayDenied()        — UPFBuildComponent on placement denial (pkg-building)
 */
UCLASS()
class PAINTFORGE_API UPFCombatAudio : public UActorComponent
{
	GENERATED_BODY()

public:
	UPFCombatAudio();

	// v1 bodies: pitch-shifted engine default sound or no-op (contract §3.4).
	void PlayHitmarker();
	void PlayElim();
	void PlayMuzzle();
	void PlaySplatIncoming();
	void PlayBreakout();
	void PlayDenied();
};
