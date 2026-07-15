// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Combat/PFSplatSubsystem.h"

#include "CombatForge.h"
#include "Combat/PFCombatAudio.h"
#include "Combat/PFPaintballProjectile.h"
#include "Core/CombatForgeGameState.h"
#include "Player/CombatForgeCharacter.h"
#include "GameFramework/PlayerController.h"
#include "Components/DecalComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPtr.h"

namespace
{
	constexpr float PendingBrightness = 0.6f;      // 60% variant (contract §3.4)
	constexpr float PendingLifetimeSec = 0.6f;     // unconfirmed window (04 §5.2)
	constexpr float PendingFadeOutSec = 0.2f;      // 04 §5.2 fade (closes prior CONTRACT-GAP)
	// Airsoft BB impact scuff — small pockmark, not a paint disc. DecalSize X = projection depth.
	constexpr float SplatSizeUU = 14.f;
	constexpr float SplatProjectionDepthUU = 8.f;
	constexpr float SplatNormalOffsetUU = 0.5f;
	constexpr float JitterMin = 0.75f;
	constexpr float JitterMax = 1.35f;
	constexpr float AspectMin = 0.72f;             // slight stretch → ricochet / scrape variety
	constexpr float AspectMax = 1.28f;
	constexpr float PuffSizeUU = 0.07f;            // sphere scale (~7 uu) per dust wisp
	constexpr float PuffNormalOffsetUU = 3.f;
	constexpr float PuffEmissiveStrength = 0.35f;  // was 0.95 — dust reads as dust, not a glowing orange blob

	// Soft paths — CDO FObjectFinder is only reliable for /Engine content (playbook §2).
	TSoftObjectPtr<UMaterialInterface> SplatDecalMatRef(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ImpactMark.M_PF_ImpactMark")));
	TSoftObjectPtr<UMaterialInterface> ImpactDustMatRef(
		FSoftObjectPath(TEXT("/Game/Materials/M_PF_ImpactDust.M_PF_ImpactDust")));
	TSoftObjectPtr<UNiagaraSystem> ImpactFXRef(
		FSoftObjectPath(TEXT("/Game/FX/NS_Impact.NS_Impact")));
	// Fab NiagaraExamples pack (default install path under Content/NiagaraExamples).
	TSoftObjectPtr<UNiagaraSystem> ImpactFXPackRef(
		FSoftObjectPath(TEXT("/Game/NiagaraExamples/FX_Weapons/Impacts/NS_Impact_Concrete.NS_Impact_Concrete")));
	TSoftObjectPtr<UNiagaraSystem> LyraImpactFXRef(
		FSoftObjectPath(TEXT("/Game/FX/Lyra/NS_ImpactConcrete.NS_ImpactConcrete")));

	int32 TeamIndex(uint8 Team)
	{
		return (Team == 1) ? 1 : 0;   // 255/unassigned falls back to team A tint
	}
}

UPFSplatSubsystem::UPFSplatSubsystem()
{
	// Engine sphere for impact puffs (CDO-safe /Engine path). Materials soft-load in EnsureInfrastructure.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		PuffMesh = SphereFinder.Object;
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
		if (ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
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
	const bool bReconciledPending = (Slot != INDEX_NONE);
	if (Slot == INDEX_NONE)
	{
		Slot = TakeNextSlot();
	}
	PlaceSplat(Slot, Loc, Normal, Team, /*bPending=*/false, /*ShotIndex=*/0);
	// Dust puff only when this is a new remote/world hit — owning client already puffed on pending.
	if (!bReconciledPending)
	{
		SpawnImpactPuff(Loc, Normal, Team);
	}
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
	SpawnImpactPuff(Loc, Normal, Team);   // immediate owning-client hit juice
}

void UPFSplatSubsystem::ResetPool()
{
	for (int32 i = 0; i < SplatComps.Num(); ++i)
	{
		if (UDecalComponent* Comp = SplatComps[i])
		{
			// Cancel any in-flight fade/lifespan so the pooled component is not destroyed.
			Comp->SetFadeOut(0.f, 0.f, /*DestroyOwnerAfterFade=*/false);
			Comp->SetVisibility(false);
		}
	}
	for (FSplatMeta& M : SplatMeta)
	{
		M = FSplatMeta();
	}
	NextSlot = 0;

	for (int32 i = 0; i < PuffComps.Num(); ++i)
	{
		if (UStaticMeshComponent* Comp = PuffComps[i])
		{
			Comp->SetVisibility(false);
		}
	}
	for (FPuffMeta& P : PuffMeta)
	{
		P = FPuffMeta();
	}
	NextPuffSlot = 0;

	UE_LOG(CombatForgeLog, Log, TEXT("Splat pool reset (BuildPhase entry, T20)"));
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
	if (PuffMeta.Num() != PuffPoolSize)
	{
		PuffMeta.SetNum(PuffPoolSize);
		PuffComps.SetNum(PuffPoolSize);
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

	// Load /Game materials on demand (never touch render packages on dedicated servers —
	// EnsureInfrastructure is only called from rendering spawn paths).
	if (BaseMaterial == nullptr)
	{
		BaseMaterial = SplatDecalMatRef.LoadSynchronous();
		if (BaseMaterial == nullptr)
		{
			UE_LOG(CombatForgeLog, Warning,
				TEXT("Impact decal material missing — run Scripts/gen_splat_decal_material.py"));
		}
	}
	if (DustMaterial == nullptr)
	{
		DustMaterial = ImpactDustMatRef.LoadSynchronous();
	}
	if (!bTriedImpactNiagara)
	{
		bTriedImpactNiagara = true;
		ImpactFX = ImpactFXRef.LoadSynchronous();
		if (ImpactFX == nullptr)
		{
			ImpactFX = ImpactFXPackRef.LoadSynchronous();
		}
		if (ImpactFX == nullptr)
		{
			ImpactFX = LyraImpactFXRef.LoadSynchronous();
		}
		UE_LOG(CombatForgeLog, Log, TEXT("[VFX] Niagara impact=%s"),
			ImpactFX ? TEXT("yes") : TEXT("mesh multi-puff"));
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

	if (PuffSlotMIDs.Num() != PuffPoolSize)
	{
		PuffSlotMIDs.SetNum(PuffPoolSize);
	}
}

int32 UPFSplatSubsystem::TakeNextSlot()
{
	// Round-robin: recycling the oldest is implicit (B10).
	const int32 Slot = NextSlot;
	NextSlot = (NextSlot + 1) % PoolSize;
	return Slot;
}

UDecalComponent* UPFSplatSubsystem::GetOrCreateSplatComp(int32 Index)
{
	if (!SplatComps.IsValidIndex(Index) || SplatHolder == nullptr)
	{
		return nullptr;
	}
	// SetFadeOut schedules DestroyComponent after the fade — recreate if the slot was reclaimed.
	UDecalComponent* Comp = SplatComps[Index];
	if (!IsValid(Comp))
	{
		Comp = NewObject<UDecalComponent>(SplatHolder);
		Comp->bDestroyOwnerAfterFade = false;
		Comp->SetVisibility(false);
		Comp->SetUsingAbsoluteScale(true);
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
	UDecalComponent* Comp = GetOrCreateSplatComp(Index);
	const UWorld* World = GetWorld();
	if (Comp == nullptr || World == nullptr)
	{
		return;
	}

	const int32 TIdx = TeamIndex(Team);
	if (ConfirmedMIDs.IsValidIndex(TIdx) && PendingMIDs.IsValidIndex(TIdx))
	{
		Comp->SetDecalMaterial(bPending ? PendingMIDs[TIdx].Get() : ConfirmedMIDs[TIdx].Get());
	}

	// Decals project along local X (engine DeferredDecal.usf swizzle). Align X to the surface
	// normal, then random-spin around the normal for variety (mesh era used MakeFromZ + yaw).
	FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero())
	{
		SafeNormal = FVector::UpVector;
	}
	const FQuat Align = FRotationMatrix::MakeFromX(SafeNormal).ToQuat();
	const FQuat RandomYaw(SafeNormal, FMath::FRandRange(0.f, 2.f * UE_PI));
	const float Jitter = FMath::FRandRange(JitterMin, JitterMax);
	const float Size = SplatSizeUU * Jitter;
	const float Aspect = FMath::FRandRange(AspectMin, AspectMax);
	// DecalSize = (projection half-extent along X, half-width Y, half-height Z)
	Comp->DecalSize = FVector(SplatProjectionDepthUU, Size * Aspect, Size / Aspect);

	// Cancel any prior fade/lifespan so a recycled slot is fully opaque and not destroyed.
	Comp->SetFadeOut(0.f, 0.f, /*DestroyOwnerAfterFade=*/false);
	Comp->SetWorldLocationAndRotation(Loc + SafeNormal * SplatNormalOffsetUU, (Align * RandomYaw).Rotator());
	Comp->SetVisibility(true);
	Comp->MarkRenderStateDirty();

	FSplatMeta& M = SplatMeta[Index];
	M.bActive = true;
	M.bPending = bPending;
	M.ShotIndex = ShotIndex;
	M.SpawnTime = World->GetTimeSeconds();
	M.Loc = Loc;
	M.Team = Team;
}

UStaticMeshComponent* UPFSplatSubsystem::GetOrCreatePuffComp(int32 Index)
{
	if (!PuffComps.IsValidIndex(Index) || SplatHolder == nullptr)
	{
		return nullptr;
	}
	UStaticMeshComponent* Comp = PuffComps[Index];
	if (!IsValid(Comp))
	{
		Comp = NewObject<UStaticMeshComponent>(SplatHolder);
		if (PuffMesh != nullptr)
		{
			Comp->SetStaticMesh(PuffMesh);
		}
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Comp->SetCastShadow(false);
		Comp->SetVisibility(false);
		Comp->SetUsingAbsoluteScale(true);
		Comp->RegisterComponent();
		Comp->AttachToComponent(SplatHolder->GetRootComponent(),
			FAttachmentTransformRules::KeepWorldTransform);
		PuffComps[Index] = Comp;
	}
	return Comp;
}

bool UPFSplatSubsystem::TrySpawnNiagaraImpact(const FVector& Loc, const FVector& Normal, uint8 Team)
{
	UWorld* World = GetWorld();
	if (World == nullptr || ImpactFX == nullptr)
	{
		return false;
	}
	FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero())
	{
		SafeNormal = FVector::UpVector;
	}
	// Latch User.Color before activate so team tint sticks (playbook §3).
	UNiagaraComponent* Burst = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		World, ImpactFX, Loc + SafeNormal * PuffNormalOffsetUU, SafeNormal.Rotation(),
		FVector(0.55f), /*bAutoDestroy=*/true, /*bAutoActivate=*/false,
		ENCPoolMethod::AutoRelease, /*bPreCullCheck=*/true);
	if (Burst == nullptr)
	{
		return false;
	}
	const FLinearColor TeamColor = PFColors::ForTeam(Team);
	Burst->SetVariableLinearColor(FName(TEXT("User.Color")), TeamColor);
	Burst->SetVariableLinearColor(FName(TEXT("Color")), TeamColor);
	Burst->Activate(true);
	return true;
}

void UPFSplatSubsystem::SpawnImpactPuff(const FVector& Loc, const FVector& Normal, uint8 Team)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Spatial impact thump (one per impact, via local pawn's audio component).
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (ACombatForgeCharacter* LocalChar = Cast<ACombatForgeCharacter>(PC->GetPawn()))
		{
			if (UPFCombatAudio* Audio = LocalChar->GetCombatAudio())
			{
				Audio->PlayImpactAt(Loc);
			}
		}
	}

	// Per-frame puff budget: a 90-BB frag burst lands dozens of impacts on one frame — decals and the
	// impact audio above still play per hit, but the dust wisps cap out so the storm collapses gracefully
	// instead of churning the whole pool. Single-shot impacts never hit the cap.
	if (GFrameCounter != PuffBudgetFrame)
	{
		PuffBudgetFrame = GFrameCounter;
		PuffPlacementsThisFrame = 0;
	}
	if (PuffPlacementsThisFrame > MaxPuffPlacementsPerFrame)
	{
		return;
	}

	if (TrySpawnNiagaraImpact(Loc, Normal, Team))
	{
		PuffPlacementsThisFrame += PuffsPerImpact;   // Niagara bursts spend the same budget
		return;
	}
	if (DustMaterial == nullptr)
	{
		return;
	}
	if (PuffMeta.Num() != PuffPoolSize)
	{
		PuffMeta.SetNum(PuffPoolSize);
		PuffComps.SetNum(PuffPoolSize);
	}

	FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero())
	{
		SafeNormal = FVector::UpVector;
	}
	const FQuat Align = FRotationMatrix::MakeFromZ(SafeNormal).ToQuat();
	const FVector Tangent = Align.GetAxisX();
	const FVector Bitangent = Align.GetAxisY();
	const int32 TIdx = TeamIndex(Team);
	const double Now = World->GetTimeSeconds();

	// Multi-wisp burst: core dust + two lateral flecks along the surface (reads less "one sphere").
	for (int32 W = 0; W < PuffsPerImpact; ++W)
	{
		const int32 Slot = NextPuffSlot;
		NextPuffSlot = (NextPuffSlot + 1) % PuffPoolSize;

		UStaticMeshComponent* Comp = GetOrCreatePuffComp(Slot);
		if (Comp == nullptr)
		{
			continue;
		}
		++PuffPlacementsThisFrame;
		if (DustMaterial)
		{
			if (!PuffSlotMIDs.IsValidIndex(Slot) || PuffSlotMIDs[Slot] == nullptr)
			{
				if (PuffSlotMIDs.Num() != PuffPoolSize)
				{
					PuffSlotMIDs.SetNum(PuffPoolSize);
				}
				PuffSlotMIDs[Slot] = UMaterialInstanceDynamic::Create(DustMaterial, this);
			}
			// Neutral concrete grey + a whisper of team tint (≤0.05) — the old R-dominant mix turned
			// every Team B impact into an orange blob.
			const FLinearColor TeamTint = PFColors::ForTeam(static_cast<uint8>(TIdx));
			const FLinearColor Dust(
				0.42f + TeamTint.R * 0.05f,
				0.42f + TeamTint.G * 0.05f,
				0.44f + TeamTint.B * 0.05f,
				1.f);
			PuffSlotMIDs[Slot]->SetVectorParameterValue(TEXT("EmissiveColor"), Dust);
			PuffSlotMIDs[Slot]->SetVectorParameterValue(TEXT("Color"), Dust);
			PuffSlotMIDs[Slot]->SetScalarParameterValue(TEXT("EmissiveStrength"), PuffEmissiveStrength);
			Comp->SetMaterial(0, PuffSlotMIDs[Slot]);
		}

		const float Radial = (W == 0) ? 0.f : FMath::FRandRange(3.f, 8.f);
		const float Angle = FMath::FRandRange(0.f, 2.f * UE_PI);
		const FVector Lateral = (Tangent * FMath::Cos(Angle) + Bitangent * FMath::Sin(Angle)) * Radial;
		const float Size = PuffSizeUU * FMath::FRandRange(0.7f, 1.35f) * (W == 0 ? 1.15f : 0.75f);
		Comp->SetWorldLocationAndRotation(
			Loc + SafeNormal * (PuffNormalOffsetUU + W * 1.5f) + Lateral, Align.Rotator());
		// Flatten along surface normal so it reads as a dust burst, not a ball.
		Comp->SetWorldScale3D(FVector(Size * 1.5f, Size * 1.5f, Size * 0.45f));
		Comp->SetVisibility(true);

		FPuffMeta& P = PuffMeta[Slot];
		P.bActive = true;
		P.SpawnTime = Now;
		P.HideAt = Now + PuffLifetimeSec * FMath::FRandRange(0.85f, 1.15f);
		P.StartScale = Size;
		P.EndScale = Size * (W == 0 ? 2.4f : 1.8f);
		P.TeamIdx = static_cast<uint8>(TIdx);
	}
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
			// 0.2 s alpha fade (04 §5.2). DestroyOwnerAfterFade MUST stay false — the
			// component is pooled on a shared holder actor. After the fade, LifeSpanCallback
			// DestroyComponent's the decal; GetOrCreateSplatComp recreates on next use.
			if (UDecalComponent* Comp = SplatComps.IsValidIndex(i) ? SplatComps[i].Get() : nullptr)
			{
				if (IsValid(Comp))
				{
					Comp->SetFadeOut(0.f, PendingFadeOutSec, /*DestroyOwnerAfterFade=*/false);
				}
			}
			M = FSplatMeta();
		}
	}

	// Animate + hide dust puffs (expand along surface, fade emissive, then recycle).
	for (int32 i = 0; i < PuffMeta.Num(); ++i)
	{
		FPuffMeta& P = PuffMeta[i];
		if (!P.bActive)
		{
			continue;
		}
		UStaticMeshComponent* Comp = PuffComps.IsValidIndex(i) ? PuffComps[i].Get() : nullptr;
		if (!IsValid(Comp))
		{
			P = FPuffMeta();
			continue;
		}
		if (Now >= P.HideAt)
		{
			Comp->SetVisibility(false);
			P = FPuffMeta();
			continue;
		}
		const float Life = FMath::Max(0.001f, static_cast<float>(P.HideAt - P.SpawnTime));
		const float T = FMath::Clamp(static_cast<float>(Now - P.SpawnTime) / Life, 0.f, 1.f);
		const float S = FMath::Lerp(P.StartScale, P.EndScale, T);
		Comp->SetWorldScale3D(FVector(S * 1.5f, S * 1.5f, S * 0.45f));
		// Drift slightly off the surface.
		Comp->AddWorldOffset(Comp->GetUpVector() * (12.f * 0.1f));
		if (PuffSlotMIDs.IsValidIndex(i) && PuffSlotMIDs[i])
		{
			PuffSlotMIDs[i]->SetScalarParameterValue(TEXT("EmissiveStrength"),
				PuffEmissiveStrength * (1.f - T * T));
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
	if (ACombatForgeGameState* GS = World->GetGameState<ACombatForgeGameState>())
	{
		GS->OnPhaseChangedEvent.AddUObject(this, &UPFSplatSubsystem::HandlePhaseChanged);
		bBoundToGameState = true;
		World->GetTimerManager().ClearTimer(GameStateBindTimer);
	}
}
