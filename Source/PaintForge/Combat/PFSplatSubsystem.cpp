// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFSplatSubsystem.h"

#include "PaintForge.h"
#include "Combat/PFPaintballProjectile.h"
#include "Core/PaintForgeGameState.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float PendingBrightness = 0.6f;      // 60% variant (contract §3.4)
	constexpr float PendingLifetimeSec = 0.6f;     // unconfirmed window (04 §5.2)
	constexpr float SplatDiscScale = 0.4f;         // (0.4, 0.4, 0.02) disc (02 §3.4)
	constexpr float SplatDiscThickness = 0.02f;
	constexpr float SplatNormalOffsetUU = 1.f;
	constexpr float JitterMin = 0.8f;
	constexpr float JitterMax = 1.3f;

	int32 TeamIndex(uint8 Team)
	{
		return (Team == 1) ? 1 : 0;   // 255/unassigned falls back to team A tint
	}
}

UPFSplatSubsystem::UPFSplatSubsystem()
{
	// CDO-time engine-asset references (02 D10) so the cooker packages them.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		SplatMesh = SphereFinder.Object;
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		BaseMaterial = MaterialFinder.Object;
	}
}

bool UPFSplatSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UPFSplatSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		return;   // does nothing on dedicated/server-only worlds (contract §3.4)
	}

	InWorld.GetTimerManager().SetTimer(PendingExpiryTimer, this,
		&UPFSplatSubsystem::TickPendingExpiry, 0.1f, true);
	// GameState may not exist yet on clients — poll until it does, then bind once (T20).
	InWorld.GetTimerManager().SetTimer(GameStateBindTimer, this,
		&UPFSplatSubsystem::TryBindGameState, 0.25f, true, 0.f);
}

void UPFSplatSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PendingExpiryTimer);
		World->GetTimerManager().ClearTimer(GameStateBindTimer);
		if (APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
		{
			GS->OnPhaseChangedEvent.RemoveAll(this);
		}
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------- public API

void UPFSplatSubsystem::SpawnConfirmedSplat(const FVector& Loc, const FVector& Normal, uint8 Team)
{
	if (!IsRenderingWorld())
	{
		return;
	}
	EnsureInfrastructure();

	// Owning-client reconcile (04 §5.2): a pending splat within 75 uu is consumed by this
	// confirmation — re-place it at the server-confirmed location at full brightness.
	int32 Slot = INDEX_NONE;
	for (int32 i = 0; i < SplatMeta.Num(); ++i)
	{
		const FSplatMeta& M = SplatMeta[i];
		if (M.bActive && M.bPending &&
			FVector::DistSquared(M.Loc, Loc) <= FMath::Square(ReconcileRadiusUU))
		{
			Slot = i;
			break;
		}
	}
	if (Slot == INDEX_NONE)
	{
		Slot = TakeNextSlot();
	}
	PlaceSplat(Slot, Loc, Normal, Team, /*bPending=*/false, /*ShotIndex=*/0);
}

void UPFSplatSubsystem::SpawnPendingSplat(uint32 ShotIndex, const FVector& Loc,
	const FVector& Normal, uint8 Team)
{
	if (!IsRenderingWorld())
	{
		return;
	}
	EnsureInfrastructure();
	PlaceSplat(TakeNextSlot(), Loc, Normal, Team, /*bPending=*/true, ShotIndex);
}

void UPFSplatSubsystem::ResetPool()
{
	for (int32 i = 0; i < SplatComps.Num(); ++i)
	{
		if (UStaticMeshComponent* Comp = SplatComps[i])
		{
			Comp->SetVisibility(false);
		}
	}
	for (FSplatMeta& M : SplatMeta)
	{
		M = FSplatMeta();
	}
	NextSlot = 0;
	UE_LOG(PaintForgeLog, Log, TEXT("Splat pool reset (BuildPhase entry, T20)"));
}

APFPaintballProjectile* UPFSplatSubsystem::AcquireCosmeticProjectile()
{
	UWorld* World = GetWorld();
	if (!IsRenderingWorld() || World == nullptr)
	{
		return nullptr;
	}
	if (CosmeticPool.Num() != CosmeticProjectilePoolSize)
	{
		CosmeticPool.SetNum(CosmeticProjectilePoolSize);
	}

	const int32 Slot = NextCosmeticSlot;
	NextCosmeticSlot = (NextCosmeticSlot + 1) % CosmeticProjectilePoolSize;

	APFPaintballProjectile* Ball = CosmeticPool[Slot];
	if (Ball == nullptr)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Ball = World->SpawnActor<APFPaintballProjectile>(APFPaintballProjectile::StaticClass(),
			FTransform::Identity, SpawnParams);
		if (Ball == nullptr)
		{
			return nullptr;
		}
		Ball->Deactivate();   // park until the caller's InitProjectile
		CosmeticPool[Slot] = Ball;
	}
	else if (Ball->IsInFlight())
	{
		Ball->Deactivate();   // round-robin recycles the oldest in-flight ball (04 §5.3)
	}
	return Ball;
}

// ---------------------------------------------------------------- internals

bool UPFSplatSubsystem::IsRenderingWorld() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_DedicatedServer;
}

void UPFSplatSubsystem::EnsureInfrastructure()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (SplatMeta.Num() != PoolSize)
	{
		SplatMeta.SetNum(PoolSize);
		SplatComps.SetNum(PoolSize);
	}

	if (SplatHolder == nullptr)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SplatHolder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity,
			SpawnParams);
		if (SplatHolder != nullptr)
		{
			USceneComponent* Root = NewObject<USceneComponent>(SplatHolder, TEXT("SplatRoot"));
			SplatHolder->SetRootComponent(Root);
			Root->RegisterComponent();
			SplatHolder->SetActorEnableCollision(false);
		}
	}

	if (ConfirmedMIDs.Num() == 0 && BaseMaterial != nullptr)
	{
		ConfirmedMIDs.SetNum(2);
		PendingMIDs.SetNum(2);
		for (int32 T = 0; T < 2; ++T)
		{
			const FLinearColor TeamColor = PFColors::ForTeam(static_cast<uint8>(T));
			ConfirmedMIDs[T] = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			ConfirmedMIDs[T]->SetVectorParameterValue(TEXT("Color"), TeamColor);
			PendingMIDs[T] = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			PendingMIDs[T]->SetVectorParameterValue(TEXT("Color"), TeamColor * PendingBrightness);
		}
	}
}

int32 UPFSplatSubsystem::TakeNextSlot()
{
	// Round-robin: recycling the oldest is implicit (B10).
	const int32 Slot = NextSlot;
	NextSlot = (NextSlot + 1) % PoolSize;
	return Slot;
}

UStaticMeshComponent* UPFSplatSubsystem::GetOrCreateSplatComp(int32 Index)
{
	if (!SplatComps.IsValidIndex(Index) || SplatHolder == nullptr)
	{
		return nullptr;
	}
	UStaticMeshComponent* Comp = SplatComps[Index];
	if (Comp == nullptr)
	{
		Comp = NewObject<UStaticMeshComponent>(SplatHolder);
		Comp->SetStaticMesh(SplatMesh);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Comp->SetCastShadow(false);
		Comp->SetVisibility(false);
		Comp->SetComponentTickEnabled(false);
		Comp->RegisterComponent();
		Comp->AttachToComponent(SplatHolder->GetRootComponent(),
			FAttachmentTransformRules::KeepWorldTransform);
		SplatComps[Index] = Comp;
	}
	return Comp;
}

void UPFSplatSubsystem::PlaceSplat(int32 Index, const FVector& Loc, const FVector& Normal,
	uint8 Team, bool bPending, uint32 ShotIndex)
{
	UStaticMeshComponent* Comp = GetOrCreateSplatComp(Index);
	const UWorld* World = GetWorld();
	if (Comp == nullptr || World == nullptr)
	{
		return;
	}

	const int32 TIdx = TeamIndex(Team);
	if (ConfirmedMIDs.IsValidIndex(TIdx) && PendingMIDs.IsValidIndex(TIdx))
	{
		Comp->SetMaterial(0, bPending ? PendingMIDs[TIdx].Get() : ConfirmedMIDs[TIdx].Get());
	}

	// Disc aligned Z-to-normal, +1 uu offset, random yaw + 0.8–1.3× jitter (02 §3.4).
	FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero())
	{
		SafeNormal = FVector::UpVector;
	}
	const FQuat Align = FRotationMatrix::MakeFromZ(SafeNormal).ToQuat();
	const FQuat RandomYaw(FVector::UpVector, FMath::FRandRange(0.f, 2.f * UE_PI));
	const float Jitter = FMath::FRandRange(JitterMin, JitterMax);
	const FVector Scale(SplatDiscScale * Jitter, SplatDiscScale * Jitter, SplatDiscThickness);

	Comp->SetWorldTransform(
		FTransform(Align * RandomYaw, Loc + SafeNormal * SplatNormalOffsetUU, Scale));
	Comp->SetVisibility(true);

	FSplatMeta& M = SplatMeta[Index];
	M.bActive = true;
	M.bPending = bPending;
	M.ShotIndex = ShotIndex;
	M.SpawnTime = World->GetTimeSeconds();
	M.Loc = Loc;
	M.Team = Team;
}

void UPFSplatSubsystem::TickPendingExpiry()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	for (int32 i = 0; i < SplatMeta.Num(); ++i)
	{
		FSplatMeta& M = SplatMeta[i];
		if (M.bActive && M.bPending && Now - M.SpawnTime > PendingLifetimeSec)
		{
			// CONTRACT-GAP: 04 §5.2 asks for a 0.2 s fade-out, but the opaque
			// BasicShapeMaterial cannot alpha-fade (same graybox concession as T7);
			// unconfirmed pendings hide instantly at the 0.6 s deadline instead.
			if (UStaticMeshComponent* Comp = SplatComps.IsValidIndex(i) ? SplatComps[i].Get() : nullptr)
			{
				Comp->SetVisibility(false);
			}
			M = FSplatMeta();
		}
	}
}

void UPFSplatSubsystem::HandlePhaseChanged(EPFMatchPhase NewPhase)
{
	if (NewPhase == EPFMatchPhase::Build)
	{
		ResetPool();   // T20: splats persist across rounds; only a fresh arena wipes them
	}
}

void UPFSplatSubsystem::TryBindGameState()
{
	UWorld* World = GetWorld();
	if (World == nullptr || bBoundToGameState)
	{
		return;
	}
	if (APaintForgeGameState* GS = World->GetGameState<APaintForgeGameState>())
	{
		GS->OnPhaseChangedEvent.AddUObject(this, &UPFSplatSubsystem::HandlePhaseChanged);
		bBoundToGameState = true;
		World->GetTimerManager().ClearTimer(GameStateBindTimer);
	}
}
