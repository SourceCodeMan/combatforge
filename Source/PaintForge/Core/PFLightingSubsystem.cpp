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

	// Airsoft / CQB field mood: overcast industrial yard — muted, readable, not bright speedball.
	// All natively spawnable, zero authored assets (T17). Local/cosmetic only — never replicate.
	if (ADirectionalLight* Sun = World.SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(),
			// Lower winter sun — long soft shadows across the 64×40 m field
			FTransform(FRotator(-38.f, -50.f, 0.f), FVector::ZeroVector), Params))
	{
		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			SunComp->SetMobility(EComponentMobility::Movable);
			SunComp->SetIntensity(3.2f);   // was 6 — dimmer outdoor-yard daylight
			SunComp->SetLightColor(FLinearColor(0.92f, 0.94f, 1.0f));   // cool overcast white
			SunComp->SetAtmosphereSunLight(true);
			SunComp->SetDynamicShadowDistanceMovableLight(16000.f);   // field-scale, not open-world
			SunComp->SetShadowBias(0.4f);
			SunComp->SetSpecularScale(0.35f);   // damp specular glitter on metal posts
		}
	}

	if (ASkyAtmosphere* Atmo = World.SpawnActor<ASkyAtmosphere>(
			ASkyAtmosphere::StaticClass(), FTransform::Identity, Params))
	{
		// Slightly hazier / lower-contrast sky so the concrete shell reads as the stage.
		if (USkyAtmosphereComponent* AtmoComp = Atmo->GetComponent())
		{
			AtmoComp->SetAtmosphereHeight(45.f);
			AtmoComp->SetMultiScatteringFactor(0.8f);
			AtmoComp->SetRayleighScattering(FLinearColor(0.14f, 0.22f, 0.55f));
			AtmoComp->SetMieScatteringScale(0.006f);
			AtmoComp->SetMieAbsorptionScale(0.0015f);
		}
	}

	if (ASkyLight* Sky = World.SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, Params))
	{
		if (USkyLightComponent* SkyComp = Sky->GetLightComponent())
		{
			SkyComp->SetMobility(EComponentMobility::Movable);
			SkyComp->SetRealTimeCapture(true);   // ambient from atmosphere (no cubemap)
			SkyComp->SetIntensity(0.85f);
			SkyComp->SetLowerHemisphereColor(FLinearColor(0.08f, 0.09f, 0.10f));
		}
	}

	if (AExponentialHeightFog* Fog = World.SpawnActor<AExponentialHeightFog>(
			AExponentialHeightFog::StaticClass(), FTransform::Identity, Params))
	{
		if (UExponentialHeightFogComponent* FogComp = Fog->GetComponent())
		{
			// Soft distance softener across 64 m field — depth without washing the play area.
			FogComp->SetFogDensity(0.018f);
			FogComp->SetFogHeightFalloff(0.12f);
			FogComp->SetFogInscatteringColor(FLinearColor(0.45f, 0.50f, 0.55f));
			FogComp->SetFogMaxOpacity(0.55f);
			FogComp->SetStartDistance(400.f);   // keep near play sharp
			FogComp->SetVolumetricFog(false);   // cheap; friends on mid PCs
		}
	}

	// Light cloud layer for sky interest (cheap; no Lumen/Nanite dependency).
	World.SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);

	if (APostProcessVolume* PPV = World.SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), FTransform::Identity, Params))
	{
		PPV->bUnbound = true;
		PPV->Priority = 1.f;
		FPostProcessSettings& PP = PPV->Settings;

		// Locked exposure (contract / prior rig) — no auto-eye adapt mid-fight.
		PP.bOverride_AutoExposureMinBrightness = true; PP.AutoExposureMinBrightness = 1.f;
		PP.bOverride_AutoExposureMaxBrightness = true; PP.AutoExposureMaxBrightness = 1.f;

		// Slightly desaturated industrial grade; keep contrast for target readability.
		PP.bOverride_BloomIntensity = true;    PP.BloomIntensity = 0.25f;
		PP.bOverride_VignetteIntensity = true; PP.VignetteIntensity = 0.42f;
		PP.bOverride_ColorSaturation = true;   PP.ColorSaturation = FVector4(0.90, 0.90, 0.92, 1.0);
		PP.bOverride_ColorContrast = true;     PP.ColorContrast = FVector4(1.08, 1.08, 1.06, 1.0);
		PP.bOverride_ColorGamma = true;        PP.ColorGamma = FVector4(1.0, 1.0, 1.02, 1.0);
		PP.bOverride_ColorGain = true;         PP.ColorGain = FVector4(0.98, 0.99, 1.02, 1.0);

		// Mild cool temperature toward warehouse yard (without crushing team colors).
		PP.bOverride_WhiteTemp = true; PP.WhiteTemp = 6200.f;
		PP.bOverride_WhiteTint = true; PP.WhiteTint = 0.05f;
	}

	UE_LOG(PaintForgeLog, Log, TEXT("PFLightingSubsystem: CQB lighting rig spawned (netmode %d)"),
		static_cast<int32>(World.GetNetMode()));
}
