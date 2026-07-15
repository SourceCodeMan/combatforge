// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Core/PFLightingSubsystem.h"

#include "CombatForge.h"
#include "Core/CombatForgeTypes.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
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

namespace
{
	// Arena geometry mirrors APFArenaShell (field origin at world 0,0; floor top Z=0).
	constexpr float FieldX = static_cast<float>(PFGrid::CellsX * PFGrid::CellUU);   // 6400
	constexpr float FieldY = static_cast<float>(PFGrid::CellsY * PFGrid::CellUU);   // 4000
	constexpr float FieldCX = FieldX * 0.5f;   // 3200
	constexpr float FieldCY = FieldY * 0.5f;   // 2000
	constexpr float CeilingZ = 1500.f;
	constexpr float BayZ = 1380.f;             // just under trusses
	constexpr float PenCX = FieldCX;
	constexpr float PenCY = -3000.f;

	FVector4 Grade(float R, float G, float B, float W = 1.f)
	{
		return FVector4(R, G, B, W);
	}

	FActorSpawnParameters MakeSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		return Params;
	}

	// UE directional/spot beams travel along the component +X axis → use aim direction's rotator.
	FRotator DirLightRotFromAim(const FVector& AimDir)
	{
		return AimDir.GetSafeNormal().Rotation();
	}

	void ConfigureDirectional(UDirectionalLightComponent* L, float Intensity, const FLinearColor& Color,
		bool bCastShadows, bool bAtmosphereSun, float SpecScale, float Indirect)
	{
		if (L == nullptr)
		{
			return;
		}
		L->SetMobility(EComponentMobility::Movable);
		L->SetIntensity(Intensity);
		L->SetLightColor(Color);
		L->SetAtmosphereSunLight(bAtmosphereSun);
		L->SetCastShadows(bCastShadows);
		L->SetSpecularScale(SpecScale);
		L->SetIndirectLightingIntensity(Indirect);
		if (bCastShadows)
		{
			L->SetDynamicShadowDistanceMovableLight(18000.f);
			L->SetShadowBias(0.28f);
			L->SetShadowSlopeBias(0.4f);
			L->SetCascadeDistributionExponent(2.8f);
			// Tighter cascades over the 64 m field so bunker edges stay crisp.
			L->SetDynamicShadowCascades(3);
			L->SetCascadeTransitionFraction(0.08f);
		}
	}

	USpotLightComponent* SpawnSpot(UWorld& World, const FVector& Loc, const FRotator& Rot,
		float IntensityCd, float AttenRadius, float InnerCone, float OuterCone,
		const FLinearColor& Color, bool bCastShadows, float Spec = 0.35f)
	{
		ASpotLight* Actor = World.SpawnActor<ASpotLight>(ASpotLight::StaticClass(),
			FTransform(Rot, Loc), MakeSpawnParams());
		if (Actor == nullptr)
		{
			return nullptr;
		}
		USpotLightComponent* L = Cast<USpotLightComponent>(Actor->GetLightComponent());
		if (L == nullptr)
		{
			return nullptr;
		}
		L->SetMobility(EComponentMobility::Movable);
		L->SetIntensityUnits(ELightUnits::Candelas);
		L->SetIntensity(IntensityCd);
		L->SetLightColor(Color);
		L->SetAttenuationRadius(AttenRadius);
		L->SetInnerConeAngle(InnerCone);
		L->SetOuterConeAngle(OuterCone);
		L->SetCastShadows(bCastShadows);
		L->SetSpecularScale(Spec);
		L->SetUseInverseSquaredFalloff(true);
		L->SetIndirectLightingIntensity(0.6f);
		if (bCastShadows)
		{
			L->SetShadowBias(0.4f);
		}
		return L;
	}

	UPointLightComponent* SpawnPoint(UWorld& World, const FVector& Loc,
		float IntensityCd, float AttenRadius, const FLinearColor& Color, bool bCastShadows)
	{
		APointLight* Actor = World.SpawnActor<APointLight>(APointLight::StaticClass(),
			FTransform(FRotator::ZeroRotator, Loc), MakeSpawnParams());
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UPointLightComponent* L = Cast<UPointLightComponent>(Actor->GetLightComponent());
		if (L == nullptr)
		{
			return nullptr;
		}
		L->SetMobility(EComponentMobility::Movable);
		L->SetIntensityUnits(ELightUnits::Candelas);
		L->SetIntensity(IntensityCd);
		L->SetLightColor(Color);
		L->SetAttenuationRadius(AttenRadius);
		L->SetCastShadows(bCastShadows);
		L->SetSpecularScale(0.25f);
		L->SetIndirectLightingIntensity(0.5f);
		L->SetSourceRadius(40.f);
		return L;
	}

	URectLightComponent* SpawnRect(UWorld& World, const FVector& Loc, const FRotator& Rot,
		float IntensityCd, float Width, float Height, float AttenRadius,
		const FLinearColor& Color, bool bCastShadows)
	{
		ARectLight* Actor = World.SpawnActor<ARectLight>(ARectLight::StaticClass(),
			FTransform(Rot, Loc), MakeSpawnParams());
		if (Actor == nullptr)
		{
			return nullptr;
		}
		URectLightComponent* L = Cast<URectLightComponent>(Actor->GetLightComponent());
		if (L == nullptr)
		{
			return nullptr;
		}
		L->SetMobility(EComponentMobility::Movable);
		L->SetIntensityUnits(ELightUnits::Candelas);
		L->SetIntensity(IntensityCd);
		L->SetLightColor(Color);
		L->SetSourceWidth(Width);
		L->SetSourceHeight(Height);
		L->SetAttenuationRadius(AttenRadius);
		L->SetBarnDoorAngle(70.f);
		L->SetBarnDoorLength(20.f);
		L->SetCastShadows(bCastShadows);
		L->SetSpecularScale(0.3f);
		L->SetIndirectLightingIntensity(0.7f);
		return L;
	}

	void ApplyFilmicPostProcess(FPostProcessSettings& PP)
	{
		// Exposure locked for competitive fights; bias tuned for the higher-contrast key/fill ratio.
		PP.bOverride_AutoExposureMethod = true;
		PP.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
		PP.bOverride_AutoExposureMinBrightness = true;
		PP.AutoExposureMinBrightness = 1.f;
		PP.bOverride_AutoExposureMaxBrightness = true;
		PP.AutoExposureMaxBrightness = 1.f;
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.05f;   // slightly darker overall → keys punch harder
		PP.bOverride_AutoExposureSpeedUp = true;
		PP.AutoExposureSpeedUp = 3.f;
		PP.bOverride_AutoExposureSpeedDown = true;
		PP.AutoExposureSpeedDown = 1.f;

		// Local exposure recovers bunker faces without flattening the whole frame.
		PP.bOverride_LocalExposureHighlightContrastScale = true;
		PP.LocalExposureHighlightContrastScale = 0.68f;
		PP.bOverride_LocalExposureShadowContrastScale = true;
		PP.LocalExposureShadowContrastScale = 0.74f;
		PP.bOverride_LocalExposureDetailStrength = true;
		PP.LocalExposureDetailStrength = 1.1f;
		PP.bOverride_LocalExposureBlurredLuminanceBlend = true;
		PP.LocalExposureBlurredLuminanceBlend = 0.6f;
		PP.bOverride_LocalExposureBlurredLuminanceKernelSizePercent = true;
		PP.LocalExposureBlurredLuminanceKernelSizePercent = 48.f;

		PP.bOverride_ToneCurveAmount = true;
		PP.ToneCurveAmount = 1.f;
		PP.bOverride_ExpandGamut = true;
		PP.ExpandGamut = 0.32f;
		PP.bOverride_FilmSlope = true;
		PP.FilmSlope = 0.91f;
		PP.bOverride_FilmToe = true;
		PP.FilmToe = 0.52f;
		PP.bOverride_FilmShoulder = true;
		PP.FilmShoulder = 0.30f;
		PP.bOverride_FilmBlackClip = true;
		PP.FilmBlackClip = 0.0f;
		PP.bOverride_FilmWhiteClip = true;
		PP.FilmWhiteClip = 0.03f;

		// Global grade — punchy, slightly cool, team paint stays readable.
		PP.bOverride_ColorSaturation = true;
		PP.ColorSaturation = Grade(0.93f, 0.93f, 0.97f);
		PP.bOverride_ColorContrast = true;
		PP.ColorContrast = Grade(1.14f, 1.14f, 1.11f);
		PP.bOverride_ColorGamma = true;
		PP.ColorGamma = Grade(0.99f, 0.99f, 1.01f);
		PP.bOverride_ColorGain = true;
		PP.ColorGain = Grade(1.00f, 0.99f, 0.97f);
		PP.bOverride_ColorOffset = true;
		PP.ColorOffset = Grade(0.001f, 0.002f, 0.004f);

		// Split tone amplifies key (warm highs) vs fill (cool lows).
		PP.bOverride_ColorSaturationShadows = true;
		PP.ColorSaturationShadows = Grade(0.86f, 0.90f, 0.98f);
		PP.bOverride_ColorContrastShadows = true;
		PP.ColorContrastShadows = Grade(1.08f, 1.08f, 1.05f);
		PP.bOverride_ColorGammaShadows = true;
		PP.ColorGammaShadows = Grade(1.01f, 1.02f, 1.05f);
		PP.bOverride_ColorGainShadows = true;
		PP.ColorGainShadows = Grade(0.94f, 0.97f, 1.06f);
		PP.bOverride_ColorOffsetShadows = true;
		PP.ColorOffsetShadows = Grade(0.003f, 0.004f, 0.010f);

		PP.bOverride_ColorSaturationMidtones = true;
		PP.ColorSaturationMidtones = Grade(0.96f, 0.96f, 0.98f);
		PP.bOverride_ColorContrastMidtones = true;
		PP.ColorContrastMidtones = Grade(1.09f, 1.09f, 1.07f);
		PP.bOverride_ColorGammaMidtones = true;
		PP.ColorGammaMidtones = Grade(1.00f, 1.00f, 1.00f);
		PP.bOverride_ColorGainMidtones = true;
		PP.ColorGainMidtones = Grade(1.01f, 1.00f, 0.98f);
		PP.bOverride_ColorOffsetMidtones = true;
		PP.ColorOffsetMidtones = Grade(0.f, 0.f, 0.f);

		PP.bOverride_ColorSaturationHighlights = true;
		PP.ColorSaturationHighlights = Grade(0.92f, 0.94f, 0.95f);
		PP.bOverride_ColorContrastHighlights = true;
		PP.ColorContrastHighlights = Grade(0.97f, 0.97f, 0.98f);
		PP.bOverride_ColorGammaHighlights = true;
		PP.ColorGammaHighlights = Grade(0.97f, 0.97f, 0.98f);
		PP.bOverride_ColorGainHighlights = true;
		PP.ColorGainHighlights = Grade(1.04f, 1.01f, 0.95f);
		PP.bOverride_ColorOffsetHighlights = true;
		PP.ColorOffsetHighlights = Grade(0.f, 0.f, 0.f);

		PP.bOverride_WhiteTemp = true;
		PP.WhiteTemp = 5850.f;
		PP.bOverride_WhiteTint = true;
		PP.WhiteTint = 0.01f;
		PP.bOverride_SceneColorTint = true;
		PP.SceneColorTint = FLinearColor(0.98f, 0.99f, 1.02f);

		// Bloom only on hot practicals / tracers.
		PP.bOverride_BloomMethod = true;
		PP.BloomMethod = EBloomMethod::BM_SOG;
		PP.bOverride_BloomIntensity = true;
		PP.BloomIntensity = 0.48f;
		PP.bOverride_BloomThreshold = true;
		PP.BloomThreshold = 1.05f;
		PP.bOverride_BloomSizeScale = true;
		PP.BloomSizeScale = 3.4f;
		PP.bOverride_Bloom1Size = true; PP.Bloom1Size = 0.3f;
		PP.bOverride_Bloom2Size = true; PP.Bloom2Size = 1.0f;
		PP.bOverride_Bloom3Size = true; PP.Bloom3Size = 2.5f;
		PP.bOverride_Bloom4Size = true; PP.Bloom4Size = 6.5f;
		PP.bOverride_Bloom5Size = true; PP.Bloom5Size = 14.f;
		PP.bOverride_Bloom6Size = true; PP.Bloom6Size = 28.f;
		PP.bOverride_Bloom1Tint = true; PP.Bloom1Tint = FLinearColor(1.00f, 0.95f, 0.88f);
		PP.bOverride_Bloom2Tint = true; PP.Bloom2Tint = FLinearColor(1.00f, 0.94f, 0.86f);
		PP.bOverride_Bloom3Tint = true; PP.Bloom3Tint = FLinearColor(0.95f, 0.94f, 1.00f);
		PP.bOverride_Bloom4Tint = true; PP.Bloom4Tint = FLinearColor(0.88f, 0.90f, 1.00f);
		PP.bOverride_Bloom5Tint = true; PP.Bloom5Tint = FLinearColor(0.82f, 0.86f, 1.00f);
		PP.bOverride_Bloom6Tint = true; PP.Bloom6Tint = FLinearColor(0.78f, 0.82f, 1.00f);

		// Stronger contact AO so bunkers read as volume under multi-light rig.
		PP.bOverride_AmbientOcclusionIntensity = true;
		PP.AmbientOcclusionIntensity = 0.62f;
		PP.bOverride_AmbientOcclusionRadius = true;
		PP.AmbientOcclusionRadius = 160.f;
		PP.bOverride_AmbientOcclusionRadiusInWS = true;
		PP.AmbientOcclusionRadiusInWS = true;
		PP.bOverride_AmbientOcclusionPower = true;
		PP.AmbientOcclusionPower = 1.7f;
		PP.bOverride_AmbientOcclusionBias = true;
		PP.AmbientOcclusionBias = 3.2f;
		PP.bOverride_AmbientOcclusionQuality = true;
		PP.AmbientOcclusionQuality = 70.f;
		PP.bOverride_AmbientOcclusionMipBlend = true;
		PP.AmbientOcclusionMipBlend = 0.55f;
		PP.bOverride_AmbientOcclusionMipScale = true;
		PP.AmbientOcclusionMipScale = 1.55f;
		PP.bOverride_AmbientOcclusionFadeDistance = true;
		PP.AmbientOcclusionFadeDistance = 9000.f;
		PP.bOverride_AmbientOcclusionFadeRadius = true;
		PP.AmbientOcclusionFadeRadius = 4500.f;
		PP.bOverride_AmbientOcclusionStaticFraction = true;
		PP.AmbientOcclusionStaticFraction = 1.f;

		PP.bOverride_VignetteIntensity = true;
		PP.VignetteIntensity = 0.34f;
		PP.bOverride_FilmGrainIntensity = true;
		PP.FilmGrainIntensity = 0.04f;
		PP.bOverride_FilmGrainIntensityShadows = true;
		PP.FilmGrainIntensityShadows = 0.6f;
		PP.bOverride_FilmGrainIntensityMidtones = true;
		PP.FilmGrainIntensityMidtones = 0.28f;
		PP.bOverride_FilmGrainIntensityHighlights = true;
		PP.FilmGrainIntensityHighlights = 0.08f;
		PP.bOverride_FilmGrainShadowsMax = true;
		PP.FilmGrainShadowsMax = 0.28f;
		PP.bOverride_FilmGrainHighlightsMin = true;
		PP.FilmGrainHighlightsMin = 0.72f;
		PP.bOverride_FilmGrainHighlightsMax = true;
		PP.FilmGrainHighlightsMax = 0.95f;
		PP.bOverride_FilmGrainTexelSize = true;
		PP.FilmGrainTexelSize = 1.15f;

		PP.bOverride_SceneFringeIntensity = true;
		PP.SceneFringeIntensity = 0.f;
		PP.bOverride_LensFlareIntensity = true;
		PP.LensFlareIntensity = 0.f;
		PP.bOverride_MotionBlurAmount = true;
		PP.MotionBlurAmount = 0.f;
	}
}

void UPFLightingSubsystem::SpawnLightingRig(UWorld& World)
{
	const FActorSpawnParameters Params = MakeSpawnParams();

	// =========================================================================
	// 1) SKY / ATMOSPHERE — cool ambient envelope (low fill = contrast headroom)
	// =========================================================================
	if (ASkyAtmosphere* Atmo = World.SpawnActor<ASkyAtmosphere>(
			ASkyAtmosphere::StaticClass(), FTransform::Identity, Params))
	{
		if (USkyAtmosphereComponent* AtmoComp = Atmo->GetComponent())
		{
			AtmoComp->SetAtmosphereHeight(48.f);
			AtmoComp->SetMultiScatteringFactor(0.7f);
			// Deeper blue Rayleigh so fill reads cooler than the warm key.
			AtmoComp->SetRayleighScattering(FLinearColor(0.11f, 0.18f, 0.48f));
			AtmoComp->SetMieScatteringScale(0.008f);
			AtmoComp->SetMieAbsorptionScale(0.002f);
			AtmoComp->SetMieAnisotropy(0.75f);
		}
	}

	World.SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);

	if (ASkyLight* Sky = World.SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, Params))
	{
		if (USkyLightComponent* SkyComp = Sky->GetLightComponent())
		{
			SkyComp->SetMobility(EComponentMobility::Movable);
			SkyComp->SetRealTimeCapture(true);
			// Intentionally LOW — flat "asset viewer" look comes from ambient that's too high.
			SkyComp->SetIntensity(0.55f);
			SkyComp->SetLightColor(FLinearColor(0.72f, 0.80f, 0.95f));   // cool sky fill
			// Dark ground bounce so floor doesn't lift the whole scene.
			SkyComp->bLowerHemisphereIsBlack = false;
			SkyComp->SetLowerHemisphereColor(FLinearColor(0.06f, 0.06f, 0.07f));
			SkyComp->bRealTimeCapture = true;
			SkyComp->OcclusionMaxDistance = 1200.f;
			SkyComp->OcclusionExponent = 1.4f;
		}
	}

	// =========================================================================
	// 2) KEY — hard warm sun / clerestory (the only heavy shadow caster for the field)
	//    Raking angle: long bunker shadows, character form, not noon-flat.
	// =========================================================================
	{
		// Aim direction: from high WNW down across the field (shadows stretch ESE).
		const FVector KeyAim = FVector(0.55f, 0.35f, -0.76f).GetSafeNormal();
		if (ADirectionalLight* Key = World.SpawnActor<ADirectionalLight>(
				ADirectionalLight::StaticClass(),
				FTransform(DirLightRotFromAim(KeyAim), FVector(FieldCX, FieldCY, 0.f)), Params))
		{
			if (UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(Key->GetLightComponent()))
			{
				ConfigureDirectional(L,
					/*Intensity=*/7.2f,
					/*Color=*/FLinearColor(1.00f, 0.93f, 0.82f),   // warm industrial daylight
					/*bCastShadows=*/true,
					/*bAtmosphereSun=*/true,
					/*Spec=*/0.55f,
					/*Indirect=*/1.15f);
				L->SetLightFunctionFadeDistance(12000.f);
				// Slight temperature shift if the property path is available via color already.
			}
#if WITH_EDITOR
			Key->SetActorLabel(TEXT("PF_KeySun"));   // editor-outliner name only; absent in packaged builds
#endif
		}
	}

	// =========================================================================
	// 3) FILL — soft cool directional opposite the key (NO shadows = free bounce)
	// =========================================================================
	{
		const FVector FillAim = FVector(-0.45f, -0.25f, -0.35f).GetSafeNormal();
		if (ADirectionalLight* Fill = World.SpawnActor<ADirectionalLight>(
				ADirectionalLight::StaticClass(),
				FTransform(DirLightRotFromAim(FillAim), FVector(FieldCX, FieldCY, 0.f)), Params))
		{
			if (UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(Fill->GetLightComponent()))
			{
				ConfigureDirectional(L,
					/*Intensity=*/1.35f,
					/*Color=*/FLinearColor(0.65f, 0.75f, 0.95f),
					/*bCastShadows=*/false,
					/*bAtmosphereSun=*/false,
					/*Spec=*/0.15f,
					/*Indirect=*/0.4f);
			}
#if WITH_EDITOR
			Fill->SetActorLabel(TEXT("PF_FillBounce"));   // editor-only outliner name
#endif
		}
	}

	// =========================================================================
	// 4) HIGH-BAY PRACTICALS — warm ceiling pools (motivated warehouse fixtures)
	//    Shadowless for perf; sun owns form shadows. Pattern = 3×2 over the field.
	// =========================================================================
	{
		const FLinearColor BayColor(1.00f, 0.94f, 0.82f);   // ~4000 K sodium-ish LED
		const float BayXs[3] = { FieldX * 0.22f, FieldCX, FieldX * 0.78f };
		const float BayYs[2] = { FieldY * 0.32f, FieldY * 0.68f };
		// Aim straight down.
		const FRotator DownRot = DirLightRotFromAim(FVector(0.f, 0.f, -1.f));
		// Spot defaults aim along +X of component after rotation — use actor rotation so -Z is beam.
		// For spots: beam is along local +X of the light in UE... Spot light points along component X.
		// Easiest: place actor at bay, rotate Pitch=-90 so +X (forward of rotator) points down.
		const FRotator SpotDown(-90.f, 0.f, 0.f);

		int32 BayIdx = 0;
		for (float X : BayXs)
		{
			for (float Y : BayYs)
			{
				const bool bCenterAisle = (FMath::Abs(X - FieldCX) < 50.f);
				// Center aisle slightly hotter — "main lane" readability.
				const float Cd = bCenterAisle ? 2800.f : 2100.f;
				const float ConeOut = bCenterAisle ? 48.f : 42.f;
				SpawnSpot(World, FVector(X, Y, BayZ), SpotDown,
					Cd, /*Atten=*/3200.f, /*Inner=*/18.f, ConeOut, BayColor,
					/*bCastShadows=*/false, /*Spec=*/0.4f);
				++BayIdx;
			}
		}

		// Soft rect panels along the long truss lines = larger soft pools (still shadowless).
		const FRotator RectDown(-90.f, 0.f, 0.f);
		const float PanelYs[2] = { FieldY * 0.25f, FieldY * 0.75f };
		for (float Y : PanelYs)
		{
			SpawnRect(World, FVector(FieldCX, Y, BayZ - 40.f), RectDown,
				/*Cd=*/900.f, /*W=*/2200.f, /*H=*/180.f, /*Atten=*/2600.f,
				FLinearColor(1.0f, 0.96f, 0.88f), /*bCastShadows=*/false);
		}
	}

	// =========================================================================
	// 5) RIM / EDGE — north & south wall wash (silhouette separation for players)
	// =========================================================================
	{
		const FLinearColor RimColor(0.75f, 0.85f, 1.00f);   // cool edge
		// Spots from outside the long walls, aiming inward and slightly down.
		const float RimZ = 900.f;
		const float RimInset = 180.f;
		// North wall (Y = FieldY): aim south (−Y) and down.
		SpawnSpot(World,
			FVector(FieldCX * 0.5f, FieldY + RimInset, RimZ),
			FRotator(-25.f, -90.f, 0.f),
			/*Cd=*/1600.f, /*Atten=*/4200.f, 20.f, 55.f, RimColor, false, 0.5f);
		SpawnSpot(World,
			FVector(FieldCX * 1.5f, FieldY + RimInset, RimZ),
			FRotator(-25.f, -90.f, 0.f),
			/*Cd=*/1600.f, /*Atten=*/4200.f, 20.f, 55.f, RimColor, false, 0.5f);
		// South wall (Y = 0): aim north (+Y) and down.
		SpawnSpot(World,
			FVector(FieldCX * 0.5f, -RimInset, RimZ),
			FRotator(-25.f, 90.f, 0.f),
			/*Cd=*/1600.f, /*Atten=*/4200.f, 20.f, 55.f, RimColor, false, 0.5f);
		SpawnSpot(World,
			FVector(FieldCX * 1.5f, -RimInset, RimZ),
			FRotator(-25.f, 90.f, 0.f),
			/*Cd=*/1600.f, /*Atten=*/4200.f, 20.f, 55.f, RimColor, false, 0.5f);
	}

	// =========================================================================
	// 6) SPAWN-END ACCENTS — subtle team-side color without washing mid-field
	//    West (team A blue strip) cool; East (team B orange strip) warm.
	// =========================================================================
	{
		// Soft points high above spawn columns — low intensity, large radius, no shadows.
		SpawnPoint(World, FVector(PFGrid::CellUU * 0.5f, FieldCY, 700.f),
			/*Cd=*/900.f, /*Atten=*/2400.f,
			FLinearColor(0.45f, 0.65f, 1.00f), false);   // blue end
		SpawnPoint(World, FVector(FieldX - PFGrid::CellUU * 0.5f, FieldCY, 700.f),
			/*Cd=*/900.f, /*Atten=*/2400.f,
			FLinearColor(1.00f, 0.55f, 0.30f), false);   // orange end

		// Extra low bounce near floor at each end (ground "kick").
		SpawnPoint(World, FVector(300.f, FieldCY, 80.f),
			/*Cd=*/350.f, /*Atten=*/1400.f,
			FLinearColor(0.55f, 0.70f, 1.00f), false);
		SpawnPoint(World, FVector(FieldX - 300.f, FieldCY, 80.f),
			/*Cd=*/350.f, /*Atten=*/1400.f,
			FLinearColor(1.00f, 0.60f, 0.35f), false);
	}

	// =========================================================================
	// 7) MIDFIELD KICK — slight floor bounce under center (aisle read)
	// =========================================================================
	SpawnPoint(World, FVector(FieldCX, FieldCY, 60.f),
		/*Cd=*/280.f, /*Atten=*/1800.f,
		FLinearColor(0.95f, 0.92f, 0.88f), false);

	// =========================================================================
	// 8) WARM-UP PEN — separate practical so lobby doesn't share fight lighting only
	// =========================================================================
	{
		const FLinearColor PenColor(1.00f, 0.95f, 0.88f);
		SpawnSpot(World, FVector(PenCX, PenCY, 600.f), FRotator(-90.f, 0.f, 0.f),
			/*Cd=*/1800.f, /*Atten=*/2200.f, 25.f, 55.f, PenColor, false);
		SpawnPoint(World, FVector(PenCX, PenCY, 120.f),
			/*Cd=*/400.f, /*Atten=*/1600.f, FLinearColor(0.8f, 0.85f, 1.0f), false);
	}

	// =========================================================================
	// 9) HEIGHT FOG — depth cue; start far so near bunkers stay crisp
	// =========================================================================
	if (AExponentialHeightFog* Fog = World.SpawnActor<AExponentialHeightFog>(
			AExponentialHeightFog::StaticClass(), FTransform::Identity, Params))
	{
		if (UExponentialHeightFogComponent* FogComp = Fog->GetComponent())
		{
			FogComp->SetFogDensity(0.009f);
			FogComp->SetFogHeightFalloff(0.16f);
			// Cool distant air — pushes warm keys forward.
			FogComp->SetFogInscatteringColor(FLinearColor(0.42f, 0.48f, 0.58f));
			FogComp->SetFogMaxOpacity(0.42f);
			FogComp->SetStartDistance(900.f);
			FogComp->SetVolumetricFog(false);
			// Directional inscatter: a touch of key warmth in sun shafts (cheap, non-volumetric).
			FogComp->SetDirectionalInscatteringColor(FLinearColor(1.0f, 0.92f, 0.78f));
			FogComp->SetDirectionalInscatteringExponent(8.f);
			FogComp->SetDirectionalInscatteringStartDistance(400.f);
		}
	}

	// =========================================================================
	// 10) FILMIC POST — locks exposure + sells the multi-light contrast
	// =========================================================================
	if (APostProcessVolume* PPV = World.SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), FTransform::Identity, Params))
	{
		PPV->bUnbound = true;
		PPV->Priority = 10.f;
		PPV->BlendWeight = 1.f;
		ApplyFilmicPostProcess(PPV->Settings);
	}

	UE_LOG(CombatForgeLog, Log,
		TEXT("PFLightingSubsystem: art-directed CQB rig (key+fill+bays+rim+spawns) netmode=%d field=%.0fx%.0f"),
		static_cast<int32>(World.GetNetMode()), FieldX, FieldY);
}
