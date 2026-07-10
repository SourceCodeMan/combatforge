// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFCombatAudio.h"

#include "PaintForge.h"

UPFCombatAudio::UPFCombatAudio()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);   // pure local cosmetics; callers are already side-correct
}

// v1: named no-ops (04 §4 — "procedural-only hooks"; the call sites are the deliverable).
// Verbose logs let the audio timeline be traced in PIE before real one-shots land.

void UPFCombatAudio::PlayHitmarker()
{
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Hitmarker (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayElim()
{
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Elim (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayMuzzle()
{
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Muzzle (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlaySplatIncoming()
{
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] SplatIncoming (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayBreakout()
{
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Breakout (%s)"), *GetNameSafe(GetOwner()));
}

void UPFCombatAudio::PlayDenied()
{
	UE_LOG(PaintForgeLog, Verbose, TEXT("[Audio] Denied (%s)"), *GetNameSafe(GetOwner()));
}
