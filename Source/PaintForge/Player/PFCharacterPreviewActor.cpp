// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterPreviewActor.h"

#include "PaintForge.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/PointLightComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Scene.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "UObject/ConstructorHelpers.h"

APFCharacterPreviewActor::APFCharacterPreviewActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Turntable = CreateDefaultSubobject<USceneComponent>(TEXT("Turntable"));
	Turntable->SetupAttachment(SceneRoot);

	// SKM_Body carries the skeleton + single-node idle anim; parts leader-pose to it (shared skeleton).
	BaseMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("PreviewBody"));
	BaseMesh->SetupAttachment(Turntable);
	// Same -90 yaw the pawn uses to face its forward; the capture sits on that +X side, so this faces it head-on.
	BaseMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	BaseMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	static ConstructorHelpers::FObjectFinder<USkeletalMesh> BodyFinder(
		TEXT("/Game/Bandits/Mesh/Body/SKM_Body.SKM_Body"));
	if (BodyFinder.Succeeded()) { BodyMeshAsset = BodyFinder.Object; }
	static ConstructorHelpers::FObjectFinder<UAnimSequence> IdleFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Idle.A_MM_Idle"));
	if (IdleFinder.Succeeded()) { IdleAnimAsset = IdleFinder.Object; }

	auto MakePart = [this](const FString& CompName) -> USkeletalMeshComponent*
	{
		USkeletalMeshComponent* C = CreateDefaultSubobject<USkeletalMeshComponent>(*CompName);
		if (C != nullptr)
		{
			C->SetupAttachment(BaseMesh);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetVisibility(false);
		}
		return C;
	};
	for (int32 i = 0; i < 2; ++i)
	{
		if (USkeletalMeshComponent* C = MakePart(FString::Printf(TEXT("PreviewBase%d"), i))) { BaseComps.Add(C); }
	}
	for (int32 i = 0; i < PFChar::SlotCount(); ++i)
	{
		if (USkeletalMeshComponent* C = MakePart(FString::Printf(TEXT("PreviewSlot%d"), i))) { SlotComps.Add(C); }
	}

	// Fixed capture rig in front of the turntable, looking back at the character. Renders ONLY this actor
	// (dark backdrop), fixed manual exposure so the frame doesn't pump as the character spins.
	Capture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("Capture"));
	Capture->SetupAttachment(SceneRoot);
	Capture->SetRelativeLocation(FVector(300.f, 0.f, 95.f));
	Capture->SetRelativeRotation(FRotator(0.f, 180.f, 0.f));
	Capture->FOVAngle = 34.f;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	// Hide the sky/atmosphere/fog so the capture shows the character on a clean dark backdrop, not the level sky.
	// (ShowOnlyList excludes arena *meshes*, but atmosphere renders as environment and leaks through otherwise.)
	auto HideFlag = [this](const TCHAR* FlagName)
	{
		FEngineShowFlagsSetting S;
		S.ShowFlagName = FlagName;
		S.Enabled = false;
		Capture->ShowFlagSettings.Add(S);
	};
	HideFlag(TEXT("Atmosphere"));
	HideFlag(TEXT("Fog"));
	HideFlag(TEXT("VolumetricFog"));
	HideFlag(TEXT("Cloud"));
	// Pin exposure (min == max) so it's stable, then compensate DOWN with a strong negative EV bias — the pin
	// alone flattened out, so the bias is the real "make it darker" lever (each -1 EV halves the brightness).
	Capture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
	Capture->PostProcessSettings.AutoExposureMinBrightness = 25.f;
	Capture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
	Capture->PostProcessSettings.AutoExposureMaxBrightness = 25.f;
	Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
	Capture->PostProcessSettings.AutoExposureBias = -3.f;   // darken ~8x; more negative = darker still

	auto MakeLight = [this](const FString& LightName, const FVector& Loc, float Lumens) -> UPointLightComponent*
	{
		UPointLightComponent* L = CreateDefaultSubobject<UPointLightComponent>(*LightName);
		if (L != nullptr)
		{
			L->SetupAttachment(SceneRoot);
			L->SetRelativeLocation(Loc);
			L->SetIntensityUnits(ELightUnits::Lumens);
			L->SetIntensity(Lumens);
			L->SetAttenuationRadius(1500.f);
			L->SetCastShadows(false);
		}
		return L;
	};
	// Gentle fill only — the level's directional sun is the key light. Keep these low so they don't wash it out.
	KeyLight  = MakeLight(TEXT("KeyLight"),  FVector(260.f, -180.f, 240.f), 4500.f);
	FillLight = MakeLight(TEXT("FillLight"), FVector(240.f,  200.f, 120.f), 1000.f);
	RimLight  = MakeLight(TEXT("RimLight"),  FVector(-160.f,  40.f, 260.f), 2500.f);
}

void APFCharacterPreviewActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureRenderTarget();
	if (Capture != nullptr)
	{
		Capture->ShowOnlyActors.Empty();
		Capture->ShowOnlyActors.Add(this);
		Capture->TextureTarget = RenderTarget;
	}
	ApplyConfig(PFChar::LoadConfig());
}

void APFCharacterPreviewActor::EnsureRenderTarget()
{
	if (RenderTarget != nullptr)
	{
		return;
	}
	RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CharPreviewRT"));
	RenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor(0.03f, 0.035f, 0.05f, 1.f);
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(512, 768);
	RenderTarget->UpdateResourceImmediate(true);
}

void APFCharacterPreviewActor::ApplyConfig(const FPFCharacterConfig& Config)
{
	if (BaseMesh == nullptr)
	{
		return;
	}

	// Mount the body once (skeleton + looping idle), then drop the feet onto the turntable plane.
	if (!bBodyMounted && BodyMeshAsset != nullptr)
	{
		BaseMesh->SetSkeletalMeshAsset(BodyMeshAsset);
		BaseMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		if (IdleAnimAsset != nullptr)
		{
			BaseMesh->PlayAnimation(IdleAnimAsset, /*bLooping=*/true);
		}
		const float MeshMinZ = BodyMeshAsset->GetBounds().GetBox().Min.Z;
		BaseMesh->SetRelativeLocation(FVector(0.f, 0.f, -MeshMinZ));
		bBodyMounted = true;
	}

	auto Mount = [this](USkeletalMeshComponent* C, USkeletalMesh* M)
	{
		if (C == nullptr)
		{
			return;
		}
		if (M != nullptr)
		{
			C->SetSkeletalMeshAsset(M);
			C->SetLeaderPoseComponent(BaseMesh);
			C->SetVisibility(true);
		}
		else
		{
			C->SetSkeletalMeshAsset(nullptr);
			C->SetVisibility(false);
		}
	};

	const TArray<FSoftObjectPath>& BaseP = PFChar::BaseParts();
	for (int32 i = 0; i < BaseComps.Num(); ++i)
	{
		USkeletalMesh* M = BaseP.IsValidIndex(i) ? Cast<USkeletalMesh>(BaseP[i].TryLoad()) : nullptr;
		Mount(BaseComps[i], M);
	}
	for (int32 s = 0; s < SlotComps.Num(); ++s)
	{
		const int32 Sel = Config.Slots.IsValidIndex(s) ? Config.Slots[s] : -1;
		USkeletalMesh* M = (Sel >= 0) ? PFChar::LoadPart(s, Sel) : nullptr;
		Mount(SlotComps[s], M);
	}
}

void APFCharacterPreviewActor::SetPreviewActive(bool bActive)
{
	bPreviewActive = bActive;
}

void APFCharacterPreviewActor::AddYaw(float DeltaDeg)
{
	SpinYaw += DeltaDeg;
	if (Turntable != nullptr)
	{
		Turntable->SetRelativeRotation(FRotator(0.f, SpinYaw, 0.f));
	}
}

void APFCharacterPreviewActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bPreviewActive)
	{
		return;
	}
	// No auto-rotation — the character stands facing the camera (subtle idle only). Rotation is user-driven via
	// AddYaw (click-drag on the tab). We still capture every frame so the idle + any drag stays live.
	if (Capture != nullptr)
	{
		Capture->CaptureScene();
	}
}
