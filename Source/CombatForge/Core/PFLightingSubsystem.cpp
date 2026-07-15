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
			// Playtest 4: skylights work but 8.5 blew the floor pools pure-white. Pulled to 6.0 — still a
			// clear sun shaft, recovers concrete detail in the pool. Fill now comes from the SkyLight.
			SunComp->SetIntensity(6.0f);
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

	// Capture point ABOVE the roof (Z=3000, over the 1800 walls / 1500 roof) in open air — from here the
	// real-time capture sees clean SKY, not the dark noisy interior. Without DFAO the captured sky-ambient
	// lights the whole interior uniformly (the roof doesn't occlude it), so the up-facing floor finally
	// gets fill. Inside the box (any Z below the walls) it captured darkness — that was the black floor.
	if (ASkyLight* Sky = World.SpawnActor<ASkyLight>(ASkyLight::StaticClass(),
			FTransform(FRotator::ZeroRotator, FVector(3200.f, 2000.f, 3000.f)), Params))
	{
		if (USkyLightComponent* SkyComp = Sky->GetLightComponent())
		{
			SkyComp->SetMobility(EComponentMobility::Movable);
			SkyComp->SetRealTimeCapture(true);
			// Ambient fill — with no GI bounce, this is the ONLY thing lighting the floor BETWEEN the sun
			// shafts, so it has to be strong or those areas go black (playtest 4). 2.2 -> 4.5 now that the
			// open roof lets the real-time capture see real sky. Lower hemisphere is a capture-independent
			// constant floor (0.24 -> 0.38) so no surface can fall to pure black.
			// 2.5 now that the capture sees real (bright) sky from above the roof — 4.5 on a bright capture
			// would blow out. This is the effective ambient floor lift; tune up if the interior's still dim.
			// 2.5 -> 2.8: skylight openings were cut in half (Tom: too many) — mild ambient bump offsets
			// the lost direct-sun pools. Brightness slider (options) gives the player final say.
			SkyComp->SetIntensity(2.8f);
			SkyComp->SetLowerHemisphereColor(FLinearColor(0.38f, 0.38f, 0.42f));
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
			// Volumetric ON so the sun through the roof skylights throws visible light shafts (god rays) —
			// the payoff that sells the openings. Density stays low (0.008) so it's shafts, not soup.
			FogComp->SetVolumetricFog(true);
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
		// Mild pull (0.50 -> 0.42) mostly to tame the sun pools; the SkyLight raise above does the real
		// lifting of the dark areas. Independent knob — raise for brighter-overall, lower if pools blow.
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.42f;

		// Clean industrial grade (original knobs + mild vignette/bloom).
		PP.bOverride_BloomIntensity = true;
		PP.BloomIntensity = 0.28f;
		PP.bOverride_BloomThreshold = true;
		PP.BloomThreshold = 1.0f;
		// Vignette darkens screen edges — cut hard (0.28 -> 0.10) so the periphery isn't crushed.
		PP.bOverride_VignetteIntensity = true;
		PP.VignetteIntensity = 0.10f;
		PP.bOverride_ColorSaturation = true;
		PP.ColorSaturation = FVector4(0.98, 0.98, 1.0, 1.0);
		// Contrast >1 crushes shadows to black — the main tonemap cause of the "everything's dark" look.
		// 1.10 -> 1.03 lifts the low end while keeping the image from going flat.
		PP.bOverride_ColorContrast = true;
		PP.ColorContrast = FVector4(1.03, 1.03, 1.02, 1.0);
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
