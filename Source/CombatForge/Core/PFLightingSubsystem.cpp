// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFLightingSubsystem.h"

#include "CombatForge.h"

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

	// ---- KNOWN-GOOD CQB RIG (restored after filmic/manual-exposure washout) ----
	// Pre-polish values that shipped readable in packaged matches. Do not reintroduce:
	//   • AEM_Manual (blew the packaged game white while menu stayed fine)
	//   • stacked fill/high-bay/rim spots (summed past the tonemapper)
	//   • custom DirLightRotFromAim (UE sun uses pitch/yaw rotator as below)

	if (ADirectionalLight* Sun = World.SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(),
			// Higher angle so the dressed ceiling + trusses cast short readable shadows
			FTransform(FRotator(-52.f, -40.f, 0.f), FVector::ZeroVector), Params))
	{
		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			SunComp->SetMobility(EComponentMobility::Movable);
			// ~40% up from the post-washout dim pass (3.6), still under the pure-white multi-light era.
			SunComp->SetIntensity(5.0f);
			SunComp->SetLightColor(FLinearColor(1.0f, 0.97f, 0.92f));
			SunComp->SetAtmosphereSunLight(true);
			SunComp->SetDynamicShadowDistanceMovableLight(16000.f);
			SunComp->SetShadowBias(0.35f);
			SunComp->SetSpecularScale(0.4f);
		}
	}

	if (ASkyAtmosphere* Atmo = World.SpawnActor<ASkyAtmosphere>(
			ASkyAtmosphere::StaticClass(), FTransform::Identity, Params))
	{
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
			SkyComp->SetRealTimeCapture(true);
			// Fill lift so bunkers aren't muddy; still below the old 1.15 wash risk with stacked lights.
			SkyComp->SetIntensity(1.10f);
			SkyComp->SetLowerHemisphereColor(FLinearColor(0.11f, 0.11f, 0.10f));
		}
	}

	if (AExponentialHeightFog* Fog = World.SpawnActor<AExponentialHeightFog>(
			AExponentialHeightFog::StaticClass(), FTransform::Identity, Params))
	{
		if (UExponentialHeightFogComponent* FogComp = Fog->GetComponent())
		{
			// Soft distance only — start farther than original 600 so near play stays clear.
			FogComp->SetFogDensity(0.008f);
			FogComp->SetFogHeightFalloff(0.14f);
			FogComp->SetFogInscatteringColor(FLinearColor(0.45f, 0.48f, 0.52f));
			FogComp->SetFogMaxOpacity(0.28f);
			FogComp->SetStartDistance(1000.f);
			FogComp->SetVolumetricFog(false);
		}
	}

	World.SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);

	if (APostProcessVolume* PPV = World.SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), FTransform::Identity, Params))
	{
		PPV->bUnbound = true;
		PPV->Priority = 1.f;
		FPostProcessSettings& PP = PPV->Settings;

		// Locked exposure — same contract as pre-polish (Histogram default, pin min=max).
		// Do NOT set AEM_Manual: packaged game went pure white with it.
		PP.bOverride_AutoExposureMinBrightness = true;
		PP.AutoExposureMinBrightness = 1.f;
		PP.bOverride_AutoExposureMaxBrightness = true;
		PP.AutoExposureMaxBrightness = 1.f;
		// Near-neutral bias — dim pass used -0.35 which read ~40% too dark in playtest.
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.05f;

		// Clean industrial grade (original knobs + mild vignette/bloom).
		PP.bOverride_BloomIntensity = true;
		PP.BloomIntensity = 0.28f;
		PP.bOverride_BloomThreshold = true;
		PP.BloomThreshold = 1.0f;
		PP.bOverride_VignetteIntensity = true;
		PP.VignetteIntensity = 0.28f;
		PP.bOverride_ColorSaturation = true;
		PP.ColorSaturation = FVector4(0.96, 0.96, 0.98, 1.0);
		PP.bOverride_ColorContrast = true;
		PP.ColorContrast = FVector4(1.10, 1.10, 1.08, 1.0);
		PP.bOverride_ColorGamma = true;
		PP.ColorGamma = FVector4(1.0, 1.0, 1.0, 1.0);
		PP.bOverride_ColorGain = true;
		PP.ColorGain = FVector4(1.0, 1.0, 1.0, 1.0);

		PP.bOverride_WhiteTemp = true;
		PP.WhiteTemp = 5800.f;
		PP.bOverride_WhiteTint = true;
		PP.WhiteTint = 0.02f;

		// Explicitly kill local-exposure lifts if project defaults enable them.
		PP.bOverride_LocalExposureBlurredLuminanceBlend = true;
		PP.LocalExposureBlurredLuminanceBlend = 0.f;
		PP.bOverride_LocalExposureHighlightContrastScale = true;
		PP.LocalExposureHighlightContrastScale = 1.f;
		PP.bOverride_LocalExposureShadowContrastScale = true;
		PP.LocalExposureShadowContrastScale = 1.f;

		PP.bOverride_MotionBlurAmount = true;
		PP.MotionBlurAmount = 0.f;
		PP.bOverride_LensFlareIntensity = true;
		PP.LensFlareIntensity = 0.f;
	}

	UE_LOG(CombatForgeLog, Log, TEXT("PFLightingSubsystem: known-good CQB rig restored (netmode %d)"),
		static_cast<int32>(World.GetNetMode()));
}
