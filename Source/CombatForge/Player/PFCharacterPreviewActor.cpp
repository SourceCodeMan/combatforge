// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Player/PFCharacterPreviewActor.h"

#include "CombatForge.h"
#include "Combat/PFWeaponCatalog.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/PointLightComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Scene.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
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
	// Prefer two-hand RIFLE idle so both hands pose for a gun (unarmed A_MM_Idle leaves the left
	// arm hanging and the weapon looks casually pointed down). Soft-fallback if the pack is missing.
	static ConstructorHelpers::FObjectFinder<UAnimSequence> RifleIdleFinder(
		TEXT("/Game/RifleAnims/Animations/BlendSpaces/Standing_IdleWalkJogRun/AS_Rifle_Idle.AS_Rifle_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> UnarmedIdleFinder(
		TEXT("/Game/Bandits/Demo/Animations/A_MM_Idle.A_MM_Idle"));
	if (RifleIdleFinder.Succeeded())
	{
		IdleAnimAsset = RifleIdleFinder.Object;
	}
	else if (UnarmedIdleFinder.Succeeded())
	{
		IdleAnimAsset = UnarmedIdleFinder.Object;
	}

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

	// Selected gun sits on the right hand (same socket resolution the live pawn uses).
	WeaponMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PreviewWeapon"));
	WeaponMeshComp->SetupAttachment(BaseMesh);
	WeaponMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WeaponMeshComp->SetCastShadow(true);
	WeaponMeshComp->SetVisibility(false);

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
	ApplyWeapon(PFWeapon::LoadConfig());
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
	// Keep the current gun mounted after clothing refresh (slot swap rebuilds leader poses).
	if (WeaponMeshComp != nullptr && WeaponMeshComp->GetStaticMesh() != nullptr)
	{
		AttachPreviewWeapon();
	}
}

void APFCharacterPreviewActor::ApplyWeapon(const FPFWeaponConfig& Config)
{
	if (WeaponMeshComp == nullptr)
	{
		return;
	}
	const FPFWeaponDef& Def = PFWeapon::Weapon(Config.Category, Config.Index);
	UStaticMesh* Wpn = PFWeapon::LoadMesh(Def);
	if (Wpn == nullptr)
	{
		WeaponMeshComp->SetStaticMesh(nullptr);
		WeaponMeshComp->SetVisibility(false);
		return;
	}
	WeaponMeshComp->SetStaticMesh(Wpn);
	// Preserve authored materials (Modern Weapons pack + Bandits); clear stale overrides.
	const int32 Mats = WeaponMeshComp->GetNumMaterials();
	for (int32 i = 0; i < Mats; ++i)
	{
		WeaponMeshComp->SetMaterial(i, nullptr);
	}
	if (UMaterialInterface* Override = PFWeapon::LoadMaterial(Def))
	{
		for (int32 i = 0; i < Mats; ++i)
		{
			WeaponMeshComp->SetMaterial(i, Override);
		}
	}
	WeaponMeshComp->SetVisibility(true);
	AttachPreviewWeapon();
}

void APFCharacterPreviewActor::AttachPreviewWeapon()
{
	if (WeaponMeshComp == nullptr || BaseMesh == nullptr || WeaponMeshComp->GetStaticMesh() == nullptr)
	{
		return;
	}
	// Same hand-socket priority list the live pawn uses (hand_r / weapon sockets).
	if (CachedWeaponBone.IsNone())
	{
		static const FName Candidates[] = {
			TEXT("hand_rSocket"), TEXT("weapon_r"), TEXT("WeaponPoint"),
			TEXT("hand_r"), TEXT("Hand_R"), TEXT("RightHand"),
			TEXT("ik_hand_gun"), TEXT("ik_hand_r"), TEXT("HandR"),
		};
		for (const FName& N : Candidates)
		{
			if (BaseMesh->DoesSocketExist(N) || BaseMesh->GetBoneIndex(N) != INDEX_NONE)
			{
				CachedWeaponBone = N;
				break;
			}
		}
	}
	if (!CachedWeaponBone.IsNone())
	{
		WeaponMeshComp->AttachToComponent(BaseMesh,
			FAttachmentTransformRules::SnapToTargetNotIncludingScale, CachedWeaponBone);
		// Two-hand combat ready in hand_r space (pairs with AS_Rifle_Idle): stock into the
		// shoulder plane, barrel level/forward — not the casual one-hand hang used for live TP.
		// Bandit hand_r: +Y is roughly palm-forward; mesh +Y is barrel-forward on our catalog guns.
		WeaponMeshComp->SetRelativeLocation(FVector(2.f, 12.f, 3.f));
		WeaponMeshComp->SetRelativeRotation(FRotator(-8.f, 95.f, 8.f));
		WeaponMeshComp->SetRelativeScale3D(FVector(0.9f));
	}
	else
	{
		// Across-chest ready if the body has no hand bone.
		WeaponMeshComp->AttachToComponent(BaseMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		WeaponMeshComp->SetRelativeLocation(FVector(12.f, 18.f, 35.f));
		WeaponMeshComp->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
		WeaponMeshComp->SetRelativeScale3D(FVector(0.9f));
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
