// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFLightingSubsystem.h"

#include "PaintForge.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"

bool UPFLightingSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UPFLightingSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		return;   // headless servers never render — no lighting rig needed
	}
	SpawnLightingRig(InWorld);
}

void UPFLightingSubsystem::SpawnLightingRig(UWorld& World)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Modern kit, all natively spawnable, zero authored assets (contract T17 keeps the level empty).
	// Spawned locally on this machine — lighting is cosmetic and must not replicate.
	if (ADirectionalLight* Sun = World.SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(),
			FTransform(FRotator(-46.f, -35.f, 0.f), FVector::ZeroVector), Params))
	{
		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			SunComp->SetMobility(EComponentMobility::Movable);
			SunComp->SetIntensity(6.f);
			SunComp->SetLightColor(FLinearColor(1.0f, 0.96f, 0.88f));
			SunComp->SetAtmosphereSunLight(true);   // drives the SkyAtmosphere sun disc + sky colour
			SunComp->SetDynamicShadowDistanceMovableLight(20000.f);
		}
	}
	World.SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FTransform::Identity, Params);   // sky + horizon
	if (ASkyLight* Sky = World.SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, Params))
	{
		if (USkyLightComponent* SkyComp = Sky->GetLightComponent())
		{
			SkyComp->SetMobility(EComponentMobility::Movable);
			SkyComp->SetRealTimeCapture(true);      // ambient bounce captured from the atmosphere (no cubemap)
			SkyComp->SetIntensity(1.f);
		}
	}
	if (AExponentialHeightFog* Fog = World.SpawnActor<AExponentialHeightFog>(
			AExponentialHeightFog::StaticClass(), FTransform::Identity, Params))
	{
		if (UExponentialHeightFogComponent* FogComp = Fog->GetComponent())
		{
			FogComp->SetFogDensity(0.015f);
		}
	}
	World.SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);   // sky detail
	if (APostProcessVolume* PPV = World.SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), FTransform::Identity, Params))
	{
		PPV->bUnbound = true;
		PPV->Priority = 1.f;
		FPostProcessSettings& PP = PPV->Settings;
		PP.bOverride_AutoExposureMinBrightness = true; PP.AutoExposureMinBrightness = 1.f;   // lock exposure
		PP.bOverride_AutoExposureMaxBrightness = true; PP.AutoExposureMaxBrightness = 1.f;
		PP.bOverride_BloomIntensity = true;            PP.BloomIntensity = 0.6f;
		PP.bOverride_VignetteIntensity = true;         PP.VignetteIntensity = 0.35f;
		PP.bOverride_ColorSaturation = true;           PP.ColorSaturation = FVector4(1.06, 1.06, 1.06, 1.0);
		PP.bOverride_ColorContrast = true;             PP.ColorContrast = FVector4(1.04, 1.04, 1.04, 1.0);
	}

	UE_LOG(PaintForgeLog, Log, TEXT("PFLightingSubsystem: lighting rig spawned locally (netmode %d)"),
		static_cast<int32>(World.GetNetMode()));
}
