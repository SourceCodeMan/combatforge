// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFCombatVFX.h"

#include "CombatForge.h"
#include "Player/CombatForgeCharacter.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "UObject/SoftObjectPtr.h"

namespace
{
	// Preferred short paths (manual renames) — then NiagaraExamples pack (Fab default install).
	// Airsoft: shots produce NO smoke — flash core + light only (the smoke path was removed in Batch C).
	TSoftObjectPtr<UNiagaraSystem> MuzzleFlashNSRef(
		FSoftObjectPath(TEXT("/Game/FX/NS_MuzzleFlash.NS_MuzzleFlash")));
	TSoftObjectPtr<UNiagaraSystem> MuzzleFlashPackRef(
		FSoftObjectPath(TEXT("/Game/NiagaraExamples/FX_Weapons/MuzzleFlashes/NS_MuzzleFlash.NS_MuzzleFlash")));
	TSoftObjectPtr<UNiagaraSystem> LyraMuzzleNSRef(
		FSoftObjectPath(TEXT("/Game/FX/Lyra/NS_WeaponFire_MuzzleFlash_Rifle.NS_WeaponFire_MuzzleFlash_Rifle")));

	TSoftObjectPtr<UMaterialInterface> FlashMatRef(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_Flash.M_PF_Flash")));
}

UPFCombatVFX::UPFCombatVFX()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

bool UPFCombatVFX::CanPlay() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_DedicatedServer;
}

void UPFCombatVFX::EnsureAssets()
{
	if (bAssetsReady)
	{
		return;
	}
	bAssetsReady = true;

	FlashMat = FlashMatRef.LoadSynchronous();
	SphereMesh = Cast<UStaticMesh>(
		FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")).TryLoad());

	if (!bTriedNiagara)
	{
		bTriedNiagara = true;
		NS_MuzzleFlash = MuzzleFlashNSRef.LoadSynchronous();
		if (NS_MuzzleFlash == nullptr)
		{
			NS_MuzzleFlash = MuzzleFlashPackRef.LoadSynchronous();
		}
		if (NS_MuzzleFlash == nullptr)
		{
			NS_MuzzleFlash = LyraMuzzleNSRef.LoadSynchronous();
		}
		UE_LOG(CombatForgeLog, Log, TEXT("[VFX] Niagara muzzle flash=%s (mesh fallback always ready)"),
			NS_MuzzleFlash ? TEXT("yes") : TEXT("no"));
	}
}

void UPFCombatVFX::EnsureMeshPool()
{
	UWorld* World = GetWorld();
	if (World == nullptr || VfxHolder != nullptr)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	VfxHolder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (VfxHolder == nullptr)
	{
		return;
	}
	USceneComponent* Root = NewObject<USceneComponent>(VfxHolder, TEXT("VfxRoot"));
	VfxHolder->SetRootComponent(Root);
	Root->RegisterComponent();
	VfxHolder->SetActorEnableCollision(false);

	PoolMeshes.SetNum(PoolSize);
	PoolFlashMIDs.SetNum(PoolSize);
	PoolMeta.SetNum(PoolSize);

	for (int32 i = 0; i < PoolSize; ++i)
	{
		UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(VfxHolder);
		if (SphereMesh)
		{
			Comp->SetStaticMesh(SphereMesh);
		}
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(false);
		Comp->SetVisibility(false);
		Comp->SetUsingAbsoluteScale(true);
		Comp->RegisterComponent();
		Comp->AttachToComponent(Root, FAttachmentTransformRules::KeepWorldTransform);
		PoolMeshes[i] = Comp;

		if (FlashMat)
		{
			PoolFlashMIDs[i] = UMaterialInstanceDynamic::Create(FlashMat, this);
			PoolFlashMIDs[i]->SetVectorParameterValue(TEXT("EmissiveColor"),
				FLinearColor(6.5f, 4.2f, 2.0f, 1.f));
			PoolFlashMIDs[i]->SetScalarParameterValue(TEXT("EmissiveStrength"), 2.2f);
		}
	}

	MuzzleLight = NewObject<UPointLightComponent>(VfxHolder);
	MuzzleLight->SetIntensity(0.f);
	MuzzleLight->SetVisibility(false);
	MuzzleLight->SetCastShadows(false);
	MuzzleLight->SetAttenuationRadius(180.f);
	MuzzleLight->SetLightColor(FLinearColor(1.f, 0.82f, 0.55f));
	MuzzleLight->RegisterComponent();
	MuzzleLight->AttachToComponent(Root, FAttachmentTransformRules::KeepWorldTransform);

	World->GetTimerManager().SetTimer(TickTimer, this, &UPFCombatVFX::TickMuzzleParts, 0.016f, true);
}

bool UPFCombatVFX::TrySpawnNiagaraMuzzle(const FVector& Loc, const FVector& Dir, bool bFirstPerson)
{
	UWorld* World = GetWorld();
	if (World == nullptr || NS_MuzzleFlash == nullptr)
	{
		return false;
	}

	const FVector SafeDir = Dir.GetSafeNormal();
	const FRotator Rot = SafeDir.IsNearlyZero() ? FRotator::ZeroRotator : SafeDir.Rotation();
	const float Scale = bFirstPerson ? 0.55f : 1.f;

	return UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		World, NS_MuzzleFlash, Loc, Rot, FVector(Scale),
		/*bAutoDestroy=*/true, /*bAutoActivate=*/true,
		ENCPoolMethod::AutoRelease, /*bPreCullCheck=*/true) != nullptr;
}

void UPFCombatVFX::SpawnMeshMuzzle(const FVector& Loc, const FVector& Dir, bool bFirstPerson)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	EnsureMeshPool();
	if (VfxHolder == nullptr)
	{
		return;
	}

	const FVector SafeDir = Dir.GetSafeNormal();
	const FVector Aim = SafeDir.IsNearlyZero() ? FVector::ForwardVector : SafeDir;
	const FQuat Align = FRotationMatrix::MakeFromX(Aim).ToQuat();
	const double Now = World->GetTimeSeconds();
	const float FP = bFirstPerson ? 0.55f : 1.f;

	auto Place = [&](const FVector& At, float StartS, float EndS, float Life)
	{
		const int32 Slot = NextPoolSlot;
		NextPoolSlot = (NextPoolSlot + 1) % PoolSize;
		UStaticMeshComponent* Comp = PoolMeshes.IsValidIndex(Slot) ? PoolMeshes[Slot].Get() : nullptr;
		if (Comp == nullptr)
		{
			return;
		}
		if (PoolFlashMIDs.IsValidIndex(Slot) && PoolFlashMIDs[Slot])
		{
			PoolFlashMIDs[Slot]->SetScalarParameterValue(TEXT("EmissiveStrength"), 2.2f);
			Comp->SetMaterial(0, PoolFlashMIDs[Slot]);
		}

		FMuzzlePartMeta& M = PoolMeta[Slot];
		M.bActive = true;
		M.StartScale = StartS;
		M.EndScale = EndS;
		M.HideAt = Now + Life;
		Comp->SetWorldLocationAndRotation(At, Align.Rotator());
		Comp->SetWorldScale3D(FVector(StartS * 1.8f, StartS, StartS));
		Comp->SetVisibility(true);
	};

	// Hot flash core — brief, small, slightly ahead of the barrel tip. Airsoft: no muzzle smoke, ever.
	Place(Loc + Aim * (4.f * FP),
		0.035f * FP * FMath::FRandRange(0.85f, 1.15f),
		0.055f * FP,
		FlashLifeSec);

	if (MuzzleLight)
	{
		MuzzleLight->SetWorldLocation(Loc + Aim * 6.f);
		MuzzleLight->SetIntensity(bFirstPerson ? 1200.f : 2800.f);
		MuzzleLight->SetVisibility(true);
	}
}

void UPFCombatVFX::TickMuzzleParts()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	if (MuzzleLight && MuzzleLight->IsVisible())
	{
		const float I = MuzzleLight->Intensity;
		if (I < 200.f)
		{
			MuzzleLight->SetIntensity(0.f);
			MuzzleLight->SetVisibility(false);
		}
		else
		{
			MuzzleLight->SetIntensity(I * 0.35f);
		}
	}

	for (int32 i = 0; i < PoolMeta.Num(); ++i)
	{
		FMuzzlePartMeta& M = PoolMeta[i];
		if (!M.bActive)
		{
			continue;
		}
		UStaticMeshComponent* Comp = PoolMeshes.IsValidIndex(i) ? PoolMeshes[i].Get() : nullptr;
		if (Comp == nullptr)
		{
			M.bActive = false;
			continue;
		}
		const float Remain = static_cast<float>(M.HideAt - Now);
		if (Remain <= 0.f)
		{
			Comp->SetVisibility(false);
			M.bActive = false;
			continue;
		}
		const float T = 1.f - FMath::Clamp(Remain / FMath::Max(FlashLifeSec, 0.001f), 0.f, 1.f);
		const float Scale = FMath::Lerp(M.StartScale, M.EndScale, T);
		Comp->SetWorldScale3D(FVector(Scale * 1.8f, Scale, Scale));
		if (PoolFlashMIDs.IsValidIndex(i) && PoolFlashMIDs[i])
		{
			PoolFlashMIDs[i]->SetScalarParameterValue(TEXT("EmissiveStrength"), 2.2f * (1.f - T));
		}
	}
}

void UPFCombatVFX::PlayMuzzleFX(const FVector& MuzzleLoc, const FVector& ShotDir, bool bFirstPerson)
{
	if (!CanPlay())
	{
		return;
	}
	EnsureAssets();

	if (TrySpawnNiagaraMuzzle(MuzzleLoc, ShotDir, bFirstPerson))
	{
		if (bFirstPerson)
		{
			EnsureMeshPool();
			if (MuzzleLight)
			{
				const FVector Aim = ShotDir.GetSafeNormal();
				MuzzleLight->SetWorldLocation(MuzzleLoc + Aim * 6.f);
				MuzzleLight->SetIntensity(900.f);
				MuzzleLight->SetVisibility(true);
			}
		}
		return;
	}

	if (FlashMat == nullptr)
	{
		return;
	}
	SpawnMeshMuzzle(MuzzleLoc, ShotDir, bFirstPerson);
}

void UPFCombatVFX::PlayMuzzleFXForOwner(bool bCosmeticFirstPerson)
{
	const ACombatForgeCharacter* Char = Cast<ACombatForgeCharacter>(GetOwner());
	if (Char == nullptr)
	{
		return;
	}
	const bool bFP = bCosmeticFirstPerson && Char->IsLocallyControlled() && Char->IsPlayerControlled();
	const FVector Loc = Char->GetMuzzleLocation(bFP);
	const FVector Dir = Char->GetBaseAimRotation().Vector();
	PlayMuzzleFX(Loc, Dir, bFP);
}
