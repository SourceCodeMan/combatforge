// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraShakeBase.h"
#include "PFCameraShakes.generated.h"

/**
 * Landing dip (04 §1.3): 0.12 s, 1.5 degree pitch amplitude. Played by
 * APaintForgeCharacter on falls > 300 uu. Asset-free: the shake pattern is a
 * constructor-built default subobject.
 */
UCLASS()
class PAINTFORGE_API UPFLandShake : public UCameraShakeBase
{
	GENERATED_BODY()

public:
	UPFLandShake(const FObjectInitializer& ObjectInitializer);
};

/**
 * Muzzle report (04 §4): 0.06 s, 0.3 degree pitch up + 0.15 degree random yaw.
 * Doubles as the only "recoil" in v1 — feel-only, no aim displacement.
 * Additive-safe at 12 bps (not single-instance; instances stack briefly).
 * Triggered per shot by UPFWeaponComponent (pkg-weapons).
 */
UCLASS()
class PAINTFORGE_API UPFFireShake : public UCameraShakeBase
{
	GENERATED_BODY()

public:
	UPFFireShake(const FObjectInitializer& ObjectInitializer);
};
